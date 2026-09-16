import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'
import StationMap from '../src/components/StationMap.vue'
import { MAP_SCRIPT_ID, resetMapLoader } from '../src/services/tencentMap'

/** 腾讯地图 GL JS 的最小替身：记录实例、几何数据与监听器，便于断言注册与销毁。 */
function createFakeTMap() {
  const instances = { maps: [], markerLayers: [] }

  class LatLng {
    constructor(latitude, longitude) {
      this.latitude = latitude
      this.longitude = longitude
    }
  }
  class MarkerStyle {
    constructor(options) {
      Object.assign(this, options)
    }
  }
  class Map {
    constructor(element, options) {
      this.element = element
      this.options = options
      this.destroyed = false
      this.center = options.center
      instances.maps.push(this)
    }

    setCenter(center) {
      this.center = center
    }

    destroy() {
      this.destroyed = true
    }
  }
  class MultiMarker {
    constructor(options) {
      this.options = options
      this.geometries = options.geometries
      this.handlers = {}
      this.removed = false
      instances.markerLayers.push(this)
    }

    on(event, handler) {
      this.handlers[event] = handler
    }

    off(event, handler) {
      if (this.handlers[event] === handler) delete this.handlers[event]
    }

    setGeometries(geometries) {
      this.geometries = geometries
    }

    setMap(map) {
      this.map = map
      this.removed = map === null
    }
  }

  return { TMap: { Map, MultiMarker, LatLng, MarkerStyle }, instances }
}

const stations = [
  {
    id: 1,
    name: 'NCS 中关村充电站',
    latitudeE6: 39977680,
    longitudeE6: 116316417,
    totalPriceCentPerKwh: 135,
    idleCount: 3,
    totalCount: 10,
    distanceMeter: 2300
  },
  { id: 2, name: 'NCS 朝阳公园充电站', latitudeE6: 39933700, longitudeE6: 116478300, totalPriceCentPerKwh: 140, idleCount: 1, totalCount: 6 }
]

beforeEach(() => {
  resetMapLoader()
  delete window.TMap
  vi.stubEnv('TENCENT_MAP_JS_KEY', 'js-key-1')
})

afterEach(() => {
  resetMapLoader()
  delete window.TMap
  vi.unstubAllEnvs()
})

describe('StationMap 三态与降级', () => {
  it('先渲染 loading，地图就绪后渲染 ready 并绘制站点标记', async () => {
    const fake = createFakeTMap()
    window.TMap = fake.TMap

    const wrapper = mount(StationMap, { props: { stations } })
    expect(wrapper.get('[data-testid="station-map"]').attributes('data-state')).toBe('loading')
    expect(wrapper.find('[data-testid="station-map-loading"]').exists()).toBe(true)

    await flushPromises()

    expect(wrapper.get('[data-testid="station-map"]').attributes('data-state')).toBe('ready')
    expect(wrapper.find('[data-testid="station-map-ready"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="station-map-loading"]').exists()).toBe(false)
    expect(wrapper.emitted('ready')).toHaveLength(1)

    const layer = fake.instances.markerLayers[0]
    expect(layer.geometries.map(geometry => geometry.id)).toEqual(['1', '2'])
    expect(layer.geometries[0].position).toBeInstanceOf(fake.TMap.LatLng)

    wrapper.unmount()
  })

  it('用户定位为 WGS-84 时转换为 GCJ-02 后居中', async () => {
    const fake = createFakeTMap()
    window.TMap = fake.TMap

    const wrapper = mount(StationMap, {
      props: { stations, userLocation: { latitudeE6: 39977680, longitudeE6: 116316417, coordinateType: 'wgs84' } }
    })
    await flushPromises()

    const map = fake.instances.maps[0]
    expect(map.center.latitude).toBeCloseTo(39.9789721, 6)
    expect(map.center.longitude).toBeCloseTo(116.3225334, 6)

    wrapper.unmount()
  })

  it('点击标记向父组件抛出 marker-click 与站点 id，卸载时注销监听并销毁地图', async () => {
    const fake = createFakeTMap()
    window.TMap = fake.TMap

    const wrapper = mount(StationMap, { props: { stations } })
    await flushPromises()

    const layer = fake.instances.markerLayers[0]
    expect(typeof layer.handlers.click).toBe('function')
    layer.handlers.click({ geometry: { id: '2' } })
    expect(wrapper.emitted('marker-click')).toEqual([['2']])

    wrapper.unmount()
    expect(layer.handlers.click).toBeUndefined()
    expect(layer.removed).toBe(true)
    expect(fake.instances.maps[0].destroyed).toBe(true)
  })

  it('缺少腾讯地图 Key 时给出明确指引、重试按钮，站点列表不受影响', async () => {
    vi.stubEnv('TENCENT_MAP_JS_KEY', '')

    const wrapper = mount(StationMap, { props: { stations } })
    await flushPromises()

    expect(wrapper.get('[data-testid="station-map"]').attributes('data-state')).toBe('error')
    const errorBox = wrapper.get('[data-testid="station-map-error"]')
    expect(errorBox.text()).toContain('尚未配置腾讯地图 Key')
    expect(wrapper.find('[data-testid="station-map-missing-key"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="station-map-retry"]').exists()).toBe(true)
    expect(wrapper.emitted('error')).toHaveLength(1)
    // 地图失败不影响站点数据：组件不修改 props。
    expect(wrapper.props('stations')).toHaveLength(2)
    expect(document.getElementById(MAP_SCRIPT_ID)).toBeNull()
  })

  it('脚本加载失败后可见失败原因，点击重试可以恢复为 ready', async () => {
    const wrapper = mount(StationMap, { props: { stations } })
    await flushPromises()
    // 脚本仍处于待加载状态。
    expect(wrapper.get('[data-testid="station-map"]').attributes('data-state')).toBe('loading')

    document.getElementById(MAP_SCRIPT_ID).dispatchEvent(new Event('error'))
    await flushPromises()

    expect(wrapper.get('[data-testid="station-map"]').attributes('data-state')).toBe('error')
    expect(wrapper.get('[data-testid="station-map-error-message"]').text()).toContain('无法连接腾讯地图服务')

    const fake = createFakeTMap()
    window.TMap = fake.TMap
    await wrapper.get('[data-testid="station-map-retry"]').trigger('click')
    await flushPromises()

    expect(wrapper.get('[data-testid="station-map"]').attributes('data-state')).toBe('ready')
    expect(wrapper.find('[data-testid="station-map-error"]').exists()).toBe(false)
    expect(fake.instances.markerLayers[0].geometries).toHaveLength(2)

    wrapper.unmount()
  })

  it('站点列表变化时刷新标记几何数据', async () => {
    const fake = createFakeTMap()
    window.TMap = fake.TMap

    const wrapper = mount(StationMap, { props: { stations: [stations[0]] } })
    await flushPromises()
    expect(fake.instances.markerLayers[0].geometries).toHaveLength(1)

    await wrapper.setProps({ stations, highlightId: 2 })
    await flushPromises()

    const layer = fake.instances.markerLayers[0]
    expect(layer.geometries).toHaveLength(2)
    expect(layer.geometries[1].styleId).toBe('highlight')

    wrapper.unmount()
  })
})
