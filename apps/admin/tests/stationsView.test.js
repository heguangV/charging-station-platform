/**
 * 站点页整页渲染回归测试。
 *
 * 背景：设备数（chargerCount）列调用了 formatInt，但 StationsView 从未导入它。
 * 该格式化函数只在**有数据到达**时才执行，所以组件挂载成功、构建也成功，
 * 只有真实数据渲染那一刻才抛 ReferenceError；渲染失败会把 vnode 树留在半成品状态，
 * 之后任何一次卸载都会连带抛错（vnode is null / parentNode of null），
 * 表现为“从站点页切不出去、内容区不更新”。
 *
 * 因此这里断言的是“表格真的把行渲染出来了”，而不只是“组件能挂载”。
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import StationsView from '../src/views/StationsView.vue'
import { vReveal } from '../src/directives/reveal'
import { useAuthStore } from '../src/stores/auth'
import { flush, installFetch, okResponse, settle } from './helpers'

/** Go Station 契约字段（无 adcode / enabled / version）。 */
const STATION = {
  id: 1,
  code: 'ST-DEV-01',
  name: '开发充电站',
  address: '成都市高新区天府软件园',
  status: 'OPEN',
  latitudeE6: 30545200,
  longitudeE6: 104070800,
  chargerCount: 2,
  idleChargerCount: 2,
  minPriceCentPerKwh: 150
}

/** 全局费率：configurations 里同时覆盖单一费率与谷时费率两种行。 */
const TARIFFS = {
  chargerCount: 100002,
  configurationCount: 2,
  minElectricityPriceCentPerKwh: 100,
  maxElectricityPriceCentPerKwh: 120,
  minServicePriceCentPerKwh: 40,
  maxServicePriceCentPerKwh: 50,
  offPeakChargerCount: 40002,
  flatChargerCount: 60000,
  configurations: [
    { electricityPriceCentPerKwh: 100, servicePriceCentPerKwh: 40, chargerCount: 60000 },
    {
      electricityPriceCentPerKwh: 120,
      servicePriceCentPerKwh: 50,
      chargerCount: 40002,
      offPeakElectricityPriceCentPerKwh: 80,
      offPeakStartHour: 22,
      offPeakEndHour: 6
    }
  ]
}

function stubMatchMedia() {
  window.matchMedia = vi.fn(() => ({
    matches: false,
    media: '(prefers-reduced-motion: reduce)',
    addEventListener: () => {},
    removeEventListener: () => {},
    addListener: () => {},
    removeListener: () => {}
  }))
}

async function mountStationsView() {
  const renderErrors = []
  stubMatchMedia()
  const pinia = createPinia()
  setActivePinia(pinia)
  const auth = useAuthStore()
  auth.token = 'admin-token'
  auth.admin = { id: 1, username: 'admin', roles: ['SUPER_ADMIN'], status: 1, mustChangePassword: false, version: 1 }

  installFetch([okResponse({ items: [STATION], meta: { page: 1, pageSize: 20, total: 1 } }), okResponse(TARIFFS)])
  const wrapper = mount(StationsView, {
    global: {
      plugins: [pinia],
      directives: { reveal: vReveal },
      config: {
        errorHandler: error => renderErrors.push(error)
      }
    }
  })
  await flush(10)
  await settle(4)
  return { wrapper, renderErrors }
}

beforeEach(() => {
  sessionStorage.clear()
  localStorage.clear()
})

afterEach(() => {
  delete window.matchMedia
  vi.unstubAllGlobals()
})

describe('站点页表格渲染', () => {
  it('站点表与费率表都把行渲染出来，不被格式化函数中断', async () => {
    const { wrapper, renderErrors } = await mountStationsView()

    // 费率表：设备数走 formatInt，谷时电费走 formatAmount + 时间窗拼接
    const tariffs = wrapper.get('[data-testid="tariffs-table"]')
    expect(tariffs.text()).toContain('60000')
    expect(tariffs.text()).toContain('40002')
    expect(tariffs.text()).toContain('0.80 元（22:00-06:00）')
    // 单一费率行没有谷时字段：DataTable 在调用 format 之前就把空值换成占位符，不会出现 NaN
    expect(tariffs.text()).toContain('—')
    expect(tariffs.text()).not.toContain('NaN')

    // 站点表：Go 契约字段直接落到列上
    const stations = wrapper.get('[data-testid="stations-table"]')
    expect(stations.text()).toContain('ST-DEV-01')
    expect(stations.text()).toContain('开发充电站')

    // 没有契约支撑的列不再占位；行内操作挂到「操作」列（原来借「版本」列渲染）
    expect(wrapper.findAll('[data-testid="stations-table"] thead th').map(th => th.text())).toEqual([
      '站点编码',
      '站点名称',
      '运营状态',
      '操作'
    ])
    // Go 只返回 status：必须换算成 enabled，否则运营中的站点会显示「已停用」
    expect(wrapper.get('[data-testid="station-state-1"]').text()).toBe('运营中')
    expect(wrapper.get('[data-testid="station-state-1"]').attributes('data-tone')).toBe('ok')
    expect(wrapper.get('[data-testid="station-toggle-1"]').text()).toBe('停用')
    // enabledCount 依赖列表行的 enabled：换算前这里恒为 0
    expect(wrapper.text()).toContain('当前页运营中 1 个')

    expect(renderErrors).toEqual([])
  })
})
