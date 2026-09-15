import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useStationStore } from '../src/stores/station'

function envelope(data) {
  return { success: true, code: 0, message: '', userMessage: '', requestId: 'req-store', data }
}

function jsonResponse(body, status = 200) {
  return { ok: status >= 200 && status < 300, status, json: async () => body }
}

function stubFetch(handler) {
  const fetchMock = vi.fn(handler)
  vi.stubGlobal('fetch', fetchMock)
  return fetchMock
}

function stationItem(overrides = {}) {
  return {
    id: 1,
    code: 'ZGC',
    name: 'NCS 中关村充电站',
    address: '北京市海淀区中关村大街 27 号',
    latitudeE6: 39977680,
    longitudeE6: 116316417,
    totalPriceCentPerKwh: 135,
    idleCount: 3,
    operationalCount: 9,
    totalCount: 10,
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
  it('已定位时同时提交 latitudeE6/longitudeE6/chargerType/page', async () => {
    const fetchMock = stubFetch(async () => jsonResponse(envelope({ items: [stationItem()], total: 1, page: 1, pageSize: 20, locationFallback: false })))
    const store = useStationStore()
    store.applyLocation({ latitudeE6: 39977680, longitudeE6: 116316417, coordinateType: 'wgs84' }, 'geolocation')

    await store.search({ chargerType: 1, page: 1 })

    const url = decodeURIComponent(fetchMock.mock.calls[0][0])
    expect(url).toBe('/api/v1/user/stations?latitudeE6=39977680&longitudeE6=116316417&chargerType=1&page=1&pageSize=20')
    expect(store.stations).toHaveLength(1)
    expect(store.total).toBe(1)
    expect(store.isEmpty).toBe(false)
    expect(store.error).toBe('')
  })

  it('位置未知时不下发经纬度，只提交 keyword', async () => {
    const fetchMock = stubFetch(async () => jsonResponse(envelope({ items: [], total: 0, page: 1, pageSize: 20, locationFallback: true })))
    const store = useStationStore()

    await store.search({ keyword: '北京南站', page: 1 })

    const url = decodeURIComponent(fetchMock.mock.calls[0][0])
    expect(url).toBe('/api/v1/user/stations?keyword=北京南站&page=1&pageSize=20')
    expect(url).not.toContain('latitudeE6')
    expect(store.locationFallback).toBe(true)
    expect(store.hasLocation).toBe(false)
  })

  it('chargerType 为 null（全部）时不提交该参数', async () => {
    const fetchMock = stubFetch(async () => jsonResponse(envelope({ items: [], total: 0, page: 1, pageSize: 20 })))
    const store = useStationStore()
    store.setChargerType(1)
    store.setChargerType(null)

    await store.search({ chargerType: null, page: 1 })

    expect(decodeURIComponent(fetchMock.mock.calls[0][0])).not.toContain('chargerType')
  })
})

describe('站点 store：加载 / 空数据 / 失败 状态转换', () => {
  it('查询期间 loading 为 true，完成后回到 false', async () => {
    let resolveFetch
    stubFetch(() => new Promise(resolve => (resolveFetch = resolve)))
    const store = useStationStore()

    const pending = store.search({ page: 1 })
    expect(store.loading).toBe(true)

    resolveFetch(jsonResponse(envelope({ items: [stationItem()], total: 1, page: 1, pageSize: 20 })))
    await pending

    expect(store.loading).toBe(false)
    expect(store.stations).toHaveLength(1)
  })

  it('空结果进入空状态而不是错误状态', async () => {
    stubFetch(async () => jsonResponse(envelope({ items: [], total: 0, page: 1, pageSize: 20 })))
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
      return jsonResponse(envelope({ items: [stationItem()], total: 1, page: 1, pageSize: 20 }))
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

describe('站点 store：详情、报价、评价与路线', () => {
  it('加载详情、设备、报价与评论墙', async () => {
    const fetchMock = stubFetch(async url => {
      if (url.includes('/chargers')) return jsonResponse(envelope({ items: [{ id: 8, code: 'ZGC-DC-02', chargerType: 1, powerWatt: 60000, status: 0, statusText: '空闲' }], total: 1, page: 1, pageSize: 50 }))
      if (url.includes('/quote')) return jsonResponse(envelope({ electricityPriceCentPerKwh: 85, finalServicePriceCentPerKwh: 55, totalPriceCentPerKwh: 140 }))
      if (url.includes('/reviews')) return jsonResponse(envelope({ items: [{ author: '张**', rating: 5, content: '充电方便', createdAt: 1788825600 }] }))
      return jsonResponse(envelope({ ...stationItem(), openingHours: '00:00-24:00', chargerTypes: [0, 1] }))
    })
    const store = useStationStore()

    await store.loadStation(1)
    await store.loadChargers(1, { chargerType: 1 })
    await store.loadQuote(1, 1)
    await store.loadReviews(1)

    expect(store.detail.name).toBe('NCS 中关村充电站')
    expect(store.chargers).toHaveLength(1)
    expect(store.quote.totalPriceCentPerKwh).toBe(140)
    expect(store.reviews[0].author).toBe('张**')
    expect(fetchMock.mock.calls[1][0]).toContain('/api/v1/user/stations/1/chargers?chargerType=1&pageSize=50')
  })

  it('路线规划把定位坐标作为起点并声明 wgs84', async () => {
    const fetchMock = stubFetch(async () =>
      jsonResponse(
        envelope({
          stationId: 1,
          stationName: 'NCS 中关村充电站',
          mode: 'driving',
          distanceMeter: 2300,
          durationSecond: 480,
          provider: 'TENCENT_MAP',
          locationFallback: false,
          routeFallback: false,
          browserUrl: 'https://apis.map.qq.com/uri/v1/routeplan?from=1'
        })
      )
    )
    const store = useStationStore()
    store.applyLocation({ latitudeE6: 39977680, longitudeE6: 116316417, coordinateType: 'wgs84' }, 'geolocation')

    await store.loadRoute(1, { mode: 'driving' })

    const url = decodeURIComponent(fetchMock.mock.calls[0][0])
    expect(url).toContain('/api/v1/user/stations/1/route?')
    expect(url).toContain('latitudeE6=39977680')
    expect(url).toContain('longitudeE6=116316417')
    expect(url).toContain('mode=driving')
    expect(url).toContain('coordinateType=wgs84')
    expect(store.route.routeFallback).toBe(false)
    expect(store.routeError).toBe('')
  })

  it('未定位时路线只提交 keyword，且失败时给出重新定位提示', async () => {
    const fetchMock = stubFetch(async () =>
      jsonResponse({ success: false, code: 12, message: 'map down', userMessage: '坐标转换失败，请重新定位或输入地址', data: null }, 503)
    )
    const store = useStationStore()
    store.setKeyword('北京南站')

    await store.loadRoute(1, { mode: 'walking' })

    const url = decodeURIComponent(fetchMock.mock.calls[0][0])
    expect(url).toContain('keyword=北京南站')
    expect(url).not.toContain('latitudeE6')
    expect(url).toContain('mode=walking')
    expect(store.route).toBeNull()
    expect(store.routeError).toBe('坐标转换失败，请重新定位或输入地址')
    expect(store.routeLoading).toBe(false)
  })
})
