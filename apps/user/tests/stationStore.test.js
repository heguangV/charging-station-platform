import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useStationStore } from '../src/stores/station'

function envelope(data) {
  return { success: true, code: 0, message: '', requestId: 'req-store', data }
}

/** Go 契约分页信封：items + meta。 */
function goPage(items, page = 1, pageSize = 20, total = items.length) {
  return { items, meta: { page, pageSize, total } }
}

function jsonResponse(body, status = 200) {
  return { ok: status >= 200 && status < 300, status, json: async () => body }
}

function stubFetch(handler) {
  const fetchMock = vi.fn(handler)
  vi.stubGlobal('fetch', fetchMock)
  return fetchMock
}

/** Go Station 契约字段。 */
function stationItem(overrides = {}) {
  return {
    id: 1,
    code: 'ZGC',
    name: 'NCS 中关村充电站',
    address: '北京市海淀区中关村大街 27 号',
    status: 'OPEN',
    latitudeE6: 39977680,
    longitudeE6: 116316417,
    chargerCount: 10,
    idleChargerCount: 3,
    minPriceCentPerKwh: 135,
    distanceMeter: 2300,
    ...overrides
  }
}

function stubGeolocation(success, failure) {
  Object.defineProperty(window.navigator, 'geolocation', {
    value: { getCurrentPosition: vi.fn((onSuccess, onFailure) => (success ? onSuccess(success) : onFailure(failure))) },
    configurable: true,
    writable: true
  })
}

beforeEach(() => {
  setActivePinia(createPinia())
})

afterEach(() => {
  vi.unstubAllGlobals()
  Object.defineProperty(window.navigator, 'geolocation', { value: undefined, configurable: true, writable: true })
})

describe('站点 store：附近查询参数', () => {
  it('已定位时同时提交 latitudeE6/longitudeE6/connectorType/page', async () => {
    const fetchMock = stubFetch(async () => jsonResponse(envelope(goPage([stationItem()]))))
    const store = useStationStore()
    store.applyLocation({ latitudeE6: 39977680, longitudeE6: 116316417, coordinateType: 'wgs84' }, 'geolocation')

    await store.search({ chargerType: 1, page: 1 })

    const url = decodeURIComponent(fetchMock.mock.calls[0][0])
    expect(url).toBe('/api/v1/stations?latitudeE6=39977680&longitudeE6=116316417&connectorType=DC&page=1&pageSize=20')
    expect(store.stations).toHaveLength(1)
    expect(store.total).toBe(1)
    expect(store.isEmpty).toBe(false)
    expect(store.error).toBe('')
  })

  it('位置未知时不下发经纬度，只提交 keyword', async () => {
    const fetchMock = stubFetch(async () => jsonResponse(envelope(goPage([]))))
    const store = useStationStore()

    await store.search({ keyword: '北京南站', page: 1 })

    const url = decodeURIComponent(fetchMock.mock.calls[0][0])
    expect(url).toBe('/api/v1/stations?keyword=北京南站&page=1&pageSize=20')
    expect(url).not.toContain('latitudeE6')
    // Go 契约没有 locationFallback 字段，适配层恒置 false。
    expect(store.locationFallback).toBe(false)
    expect(store.hasLocation).toBe(false)
  })

  it('chargerType 为 null（全部）时不提交该参数', async () => {
    const fetchMock = stubFetch(async () => jsonResponse(envelope(goPage([]))))
    const store = useStationStore()
    store.setChargerType(1)
    store.setChargerType(null)

    await store.search({ chargerType: null, page: 1 })

    expect(decodeURIComponent(fetchMock.mock.calls[0][0])).not.toContain('connectorType')
  })
})

describe('站点 store：加载 / 空数据 / 失败 状态转换', () => {
  it('查询期间 loading 为 true，完成后回到 false', async () => {
    let resolveFetch
    stubFetch(() => new Promise(resolve => (resolveFetch = resolve)))
    const store = useStationStore()

    const pending = store.search({ page: 1 })
    expect(store.loading).toBe(true)

    resolveFetch(jsonResponse(envelope(goPage([stationItem()]))))
    await pending

    expect(store.loading).toBe(false)
    expect(store.stations).toHaveLength(1)
  })

  it('空结果进入空状态而不是错误状态', async () => {
    stubFetch(async () => jsonResponse(envelope(goPage([]))))
    const store = useStationStore()

    await store.search({ page: 1 })

    expect(store.stations).toEqual([])
    expect(store.error).toBe('')
    expect(store.isEmpty).toBe(true)
  })

  it('失败时保留可展示的中文错误并清空数据，重试成功后恢复', async () => {
    let call = 0
    stubFetch(async () => {
      call += 1
      if (call === 1) return jsonResponse({ success: false, code: 12, message: 'map down', userMessage: '地图服务暂时不可用', data: null }, 503)
      return jsonResponse(envelope(goPage([stationItem()])))
    })
    const store = useStationStore()

    await store.search({ page: 1 })
    expect(store.stations).toEqual([])
    expect(store.error).toBe('地图服务暂时不可用')
    expect(store.loading).toBe(false)
    expect(store.isEmpty).toBe(false)

    await store.search({ page: 1 })
    expect(store.error).toBe('')
    expect(store.stations).toHaveLength(1)
  })
})

describe('站点 store：定位与手动兜底', () => {
  it('定位成功后写入 WGS-84 E6 坐标', async () => {
    stubGeolocation({ coords: { latitude: 39.97768, longitude: 116.316417, accuracy: 12 } })
    const store = useStationStore()

    const position = await store.locate()

    expect(position.coordinateType).toBe('wgs84')
    expect(store.location.status).toBe('success')
    expect(store.hasLocation).toBe(true)
    expect(store.location.latitudeE6).toBe(39977680)
    expect(store.location.longitudeE6).toBe(116316417)
  })

  it('定位被拒绝时进入 failed 状态并保留中文提示，预设位置仍可用', async () => {
    stubGeolocation(null, { code: 1 })
    const store = useStationStore()

    await store.locate()
    expect(store.location.status).toBe('failed')
    expect(store.location.error).toContain('定位权限被拒绝')
    expect(store.hasLocation).toBe(false)

    store.usePresetLocation('beijing-zhongguancun')
    expect(store.location.status).toBe('success')
    expect(store.location.source).toBe('preset')
    expect(store.location.latitudeE6).toBe(39977680)
    expect(store.hasLocation).toBe(true)
  })

  it('浏览器不支持定位时状态为 unsupported', async () => {
    Object.defineProperty(window.navigator, 'geolocation', { value: undefined, configurable: true, writable: true })
    const store = useStationStore()

    await store.locate()
    expect(store.location.status).toBe('unsupported')
    expect(store.location.error).toContain('不支持定位')
  })
})

describe('站点 store：详情、设备与评论墙（Go 契约）', () => {
  it('加载详情、设备与评论墙并归一化字段', async () => {
    const fetchMock = stubFetch(async url => {
      if (url.includes('/chargers')) {
        return jsonResponse(envelope(goPage([{ id: 8, stationId: 1, code: 'ZGC-DC-02', type: 'DC', powerWatt: 60000, status: 'IDLE' }], 1, 50)))
      }
      if (url.includes('/reviews')) {
        return jsonResponse(envelope(goPage([{ stars: 5, comment: '充电方便', author: '张**', createdAt: '2026-09-15T08:00:00Z' }])))
      }
      return jsonResponse(envelope(stationItem()))
    })
    const store = useStationStore()

    await store.loadStation(1)
    await store.loadChargers(1, { chargerType: 1 })
    await store.loadReviews(1)

    expect(store.detail.name).toBe('NCS 中关村充电站')
    // 站点详情的可用/总数由 Go 的 idleChargerCount/chargerCount 补齐。
    expect(store.detail.operationalCount).toBe(3)
    expect(store.detail.totalCount).toBe(10)
    expect(store.chargers).toHaveLength(1)
    // 设备状态与桩型归一化为旧数字码，供既有视图分支消费。
    expect(store.chargers[0].chargerType).toBe(1)
    expect(store.chargers[0].status).toBe(0)
    expect(store.chargers[0].statusText).toBe('空闲')
    expect(store.reviews[0].author).toBe('张**')
    expect(store.reviews[0].rating).toBe(5)
    expect(store.reviews[0].content).toBe('充电方便')
    // Go 契约：设备列表是 /chargers?stationId=，桩型参数为 connectorType。
    expect(fetchMock.mock.calls[1][0]).toContain('/api/v1/chargers?stationId=1&connectorType=DC&pageSize=50')
  })
})
