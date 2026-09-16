/**
 * 运营总览的展示回归测试。
 *
 * 背景：健康度曾经显示成 0.0%。原因是 store 的 getter 用 toInteger 校验一个**小数**——
 * toInteger 对非整数返回 null（它不截断），再 ?? 0 就把 98.97 变成 0。
 * 页面上"可运营 99004 / 总设备 100035"旁边写着 0.0%。
 *
 * DTO 层测试（tests/stats.test.js）断言的是 parseChargerStatusStats 保留了 70.83，
 * 恰好停在缺陷所在的那一层之外，所以这里补 store 与页面两层。
 */
import { afterEach, describe, expect, it, vi } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import OverviewView from '../src/views/OverviewView.vue'
import { useDashboardStore } from '../src/stores/dashboard'
import { formatPercent } from '../src/utils/format'
import { installFetch, okResponse } from './helpers'

// 总览页会挂 echarts 面板；测试只关心文案，图表初始化打桩。
vi.mock('../src/charts', () => ({
  init: () => ({ setOption: () => {}, resize: () => {}, dispose: () => {} }),
  chartAnimation: () => false,
  chartTheme: () => ({})
}))

/** 24 台设备里 17 台可运营 → 70.83%（非整数，正是被 toInteger 吞掉的那类值）。 */
const CHARGER_STATUS = {
  idleCount: 12,
  occupiedCount: 5,
  faultyCount: 2,
  restartingCount: 1,
  disabledCount: 4,
  operationalCount: 17,
  totalCount: 24,
  healthPercent: 70.83
}

const REVENUE = {
  items: [{ bucketStart: 1788134400, amountCent: 123456, energyMwh: 1500000, orderCount: 12 }],
  totalAmountCent: 123456,
  totalEnergyMwh: 1500000,
  totalOrderCount: 12
}

/** 按 URL 分发：总览页并发打 5 个请求（趋势/今日/本月/设备状态/注册用户）。 */
function payloadFor(url) {
  if (String(url).includes('/admin/stats/chargers')) return CHARGER_STATUS
  if (String(url).includes('/admin/stats/revenue')) return REVENUE
  if (String(url).includes('/admin/users')) return { total: 1000, items: [], page: 1, pageSize: 1 }
  return {}
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

afterEach(() => {
  delete window.matchMedia
  vi.unstubAllGlobals()
})

describe('设备健康度', () => {
  it('store 保留小数，不被整数校验吞成 0', async () => {
    installFetch([url => okResponse(payloadFor(url))])
    setActivePinia(createPinia())
    const dashboard = useDashboardStore()

    await dashboard.loadChargerStatus()

    expect(dashboard.chargerStatus.healthPercent).toBe(70.83)
    expect(dashboard.healthPercent).toBe(70.83)
    expect(dashboard.operationalCount).toBe(17)
    expect(dashboard.totalChargers).toBe(24)
  })

  it('页面按契约的两位小数展示，而不是 0.0%', async () => {
    stubMatchMedia()
    installFetch([url => okResponse(payloadFor(url))])
    const pinia = createPinia()
    setActivePinia(pinia)

    const wrapper = mount(OverviewView, { global: { plugins: [pinia] } })
    await flushPromises()
    await new Promise(resolve => setTimeout(resolve, 0))
    await wrapper.vm.$nextTick()

    expect(wrapper.get('[data-testid="overview-kpi-health-value"]').text()).toBe('70.83%')
    expect(wrapper.get('[data-testid="overview-health-label"]').text()).toContain('70.83%')
    // 可运营/总数与健康度必须自洽：17/24 = 70.83%
    expect(wrapper.get('[data-testid="overview-kpi-operational-value"]').text()).toBe('17')
    expect(formatPercent((17 / 24) * 100, { digits: 2 })).toBe('70.83%')
  })
})
