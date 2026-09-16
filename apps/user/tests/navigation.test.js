import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { createMemoryHistory, createRouter } from 'vue-router'
import NavigationView from '../src/views/NavigationView.vue'
import { buildRoutePlanUrl, navigationModeLabel, NAVIGATION_MODES } from '../src/services/navigation'
import { haversineMeter } from '../src/services/coordinate'
import { useStationStore } from '../src/stores/station'

/**
 * 导航页。服务端路线规划（A-07）未接入前，页面承诺的是跳转式导航：
 * 距离与坐标换算本地完成、跳转参数按 GCJ-02 提交、未定位时不伪造起点。
 */

/** 北京中关村（预设位置）与国贸（预设位置），均为 WGS-84。 */
const ZGC = { latitude: 39.97768, longitude: 116.316417 }
const GUOMAO = { latitude: 39.90881, longitude: 116.461024 }

describe('buildRoutePlanUrl', () => {
  it('携带起点/终点/方式并声明 GCJ-02 坐标系', () => {
    const url = buildRoutePlanUrl({
      from: { latitude: 39.97768, longitude: 116.316417, name: '我的位置' },
      to: { latitude: 39.90881, longitude: 116.461024, name: '国贸充电站' },
      mode: 'driving'
    })
    const parsed = new URL(url)
    expect(parsed.origin + parsed.pathname).toBe('https://apis.map.qq.com/uri/v1/routeplan')
    expect(parsed.searchParams.get('mode')).toBe('driving')
    expect(parsed.searchParams.get('from')).toBe('我的位置')
    expect(parsed.searchParams.get('fromcoord')).toBe('39.97768,116.316417')
    expect(parsed.searchParams.get('to')).toBe('国贸充电站')
    expect(parsed.searchParams.get('tocoord')).toBe('39.90881,116.461024')
    // 站点坐标按平台口径是 GCJ-02；显式声明，避免被当作 WGS-84 再次偏移。
    expect(parsed.searchParams.get('coord_type')).toBe('1')
    expect(parsed.searchParams.get('referer')).toBe('ncs-user-web')
  })

  it('未定位时省略起点，由腾讯地图在设备端使用当前位置', () => {
    const url = buildRoutePlanUrl({ from: null, to: { latitude: 30.5452, longitude: 104.0708 } })
    const params = new URL(url).searchParams
    expect(params.has('from')).toBe(false)
    expect(params.has('fromcoord')).toBe(false)
    expect(params.get('to')).toBe('目的地')
    expect(params.get('tocoord')).toBe('30.5452,104.0708')
  })

  it('步行方式与非法输入', () => {
    expect(navigationModeLabel('walking')).toBe('步行')
    expect(NAVIGATION_MODES.map(item => item.value)).toEqual(['driving', 'walking'])
    expect(() => buildRoutePlanUrl({ from: null, to: { latitude: 1, longitude: 2 }, mode: 'flying' })).toThrow(RangeError)
    expect(() => buildRoutePlanUrl({ from: null, to: null })).toThrow(RangeError)
    expect(() =>
      buildRoutePlanUrl({ from: { latitude: Number.NaN, longitude: 2 }, to: { latitude: 1, longitude: 2 } })
    ).toThrow(RangeError)
  })
})

describe('haversineMeter', () => {
  it('同一坐标距离为零，非法坐标拒绝计算', () => {
    expect(haversineMeter(39.9, 116.4, 39.9, 116.4)).toBe(0)
    expect(() => haversineMeter(Number.NaN, 116.4, 39.9, 116.4)).toThrow(RangeError)
  })

  it('中关村到国贸约 14.5 公里（同系坐标）', () => {
    const meter = haversineMeter(ZGC.latitude, ZGC.longitude, GUOMAO.latitude, GUOMAO.longitude)
    expect(meter).toBeGreaterThan(14200)
    expect(meter).toBeLessThan(14800)
  })
})

function envelope(data) {
  return { success: true, code: 0, message: '', requestId: 'req-nav', data }
}

function jsonResponse(body, status = 200) {
  return { ok: status >= 200 && status < 300, status, json: async () => body }
}

/** Go Station 契约字段（与 stationStore.test.js 同一口径）。 */
function stationItem(overrides = {}) {
  return {
    id: 7,
    code: 'GM',
    name: '国贸充电站',
    address: '北京市朝阳区国贸',
    status: 'OPEN',
    latitudeE6: 39908810,
    longitudeE6: 116461024,
    chargerCount: 8,
    idleChargerCount: 3,
    minPriceCentPerKwh: 120,
    ...overrides
  }
}

describe('NavigationView', () => {
  /** 视图在挂载时会真实加载详情；按 overrides stub 详情响应。 */
  function stubStationDetail(overrides = {}) {
    return vi.stubGlobal(
      'fetch',
      vi.fn(async () => jsonResponse(envelope(stationItem(overrides))))
    )
  }

  async function mountView() {
    const router = createRouter({
      history: createMemoryHistory(),
      routes: [
        { path: '/', component: { template: '<div />' } },
        { path: '/stations/:stationId', component: { template: '<div />' } },
        { path: '/stations/:stationId/navigation', component: { template: '<div />' } },
        { path: '/:pathMatch(.*)*', component: { template: '<div />' } }
      ]
    })
    await router.push('/stations/7/navigation')
    await router.isReady()
    const wrapper = mount(NavigationView, { global: { plugins: [router] } })
    return { wrapper, router }
  }

  beforeEach(() => {
    setActivePinia(createPinia())
  })

  afterEach(() => {
    vi.unstubAllGlobals()
    vi.restoreAllMocks()
  })

  it('未定位时提示可获取位置或用预设；跳转链接不带起点', async () => {
    stubStationDetail()
    const open = vi.fn()
    vi.stubGlobal('open', open)
    const { wrapper } = await mountView()
    await vi.waitFor(() => expect(wrapper.find('[data-testid="navigation-station-name"]').exists()).toBe(true))

    expect(wrapper.find('[data-testid="navigation-station-name"]').text()).toBe('国贸充电站')
    expect(wrapper.find('[data-testid="navigation-location-empty"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="navigation-distance"]').exists()).toBe(false)

    await wrapper.find('[data-testid="navigation-open"]').trigger('click')
    expect(open).toHaveBeenCalledTimes(1)
    const url = new URL(open.mock.calls[0][0])
    expect(url.searchParams.has('fromcoord')).toBe(false)
    expect(url.searchParams.get('tocoord')).toBe('39.90881,116.461024')
  })

  it('选择预设位置后本地计算 GCJ-02 距离，跳转携带起点', async () => {
    stubStationDetail()
    const open = vi.fn()
    vi.stubGlobal('open', open)
    const { wrapper } = await mountView()
    await vi.waitFor(() => expect(wrapper.find('[data-testid="navigation-station-name"]').exists()).toBe(true))

    await wrapper.find('[data-testid="navigation-preset-beijing-zhongguancun"]').trigger('click')
    await wrapper.vm.$nextTick()

    // 中关村（WGS-84）换算为 GCJ-02 后与站点（GCJ-02）同系计算，约 14.5 km。
    const distanceText = wrapper.find('[data-testid="navigation-distance"]').text()
    expect(distanceText).toMatch(/14\.\d km/)
    expect(wrapper.find('[data-testid="navigation-location-hint"]').text()).toContain('预设位置')

    await wrapper.find('[data-testid="navigation-open"]').trigger('click')
    const url = new URL(open.mock.calls[0][0])
    expect(url.searchParams.has('fromcoord')).toBe(true)
    // 起点坐标必须已是 GCJ-02：应略大于 WGS-84 原值（北京域内偏移为正）。
    const [, fromLatitude] = url.searchParams.get('fromcoord').split(',').map(Number)
    expect(fromLatitude).toBeGreaterThan(ZGC.latitude)
  })

  it('切换步行方式后按钮文案随之变化', async () => {
    stubStationDetail()
    const { wrapper } = await mountView()
    await vi.waitFor(() => expect(wrapper.find('[data-testid="navigation-open"]').exists()).toBe(true))

    expect(wrapper.find('[data-testid="navigation-open"]').text()).toContain('驾车')
    await wrapper.find('[data-testid="navigation-mode-walking"]').trigger('click')
    expect(wrapper.find('[data-testid="navigation-open"]').text()).toContain('步行')
  })

  it('站点缺少坐标时给出诚实空态并禁用跳转', async () => {
    stubStationDetail({ latitudeE6: null, longitudeE6: null })
    const { wrapper } = await mountView()
    await vi.waitFor(() => expect(wrapper.find('[data-testid="navigation-no-coords"]').exists()).toBe(true))

    expect(wrapper.find('[data-testid="navigation-no-coords"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="navigation-map-panel"]').exists()).toBe(false)
    expect(wrapper.find('[data-testid="navigation-open"]').attributes('disabled')).toBeDefined()
  })

  it('定位失败时展示可读错误并提供预设位置兜底', async () => {
    stubStationDetail()
    const { wrapper } = await mountView()
    await vi.waitFor(() => expect(wrapper.find('[data-testid="navigation-locate"]').exists()).toBe(true))

    // jsdom 无 geolocation：服务以 UNSUPPORTED 拒绝，页面不得静默。
    await wrapper.find('[data-testid="navigation-locate"]').trigger('click')
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-testid="navigation-location-error"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="navigation-presets"]').exists()).toBe(true)
  })
})
