import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { GeolocationError, GeolocationErrorKind, PRESET_LOCATIONS, getCurrentLocation, isGeolocationSupported, presetLocation } from '../src/services/geolocation'

function stubGeolocation(implementation) {
  Object.defineProperty(window.navigator, 'geolocation', { value: implementation, configurable: true, writable: true })
}

function removeGeolocation() {
  Object.defineProperty(window.navigator, 'geolocation', { value: undefined, configurable: true, writable: true })
}

afterEach(() => {
  removeGeolocation()
  vi.unstubAllGlobals()
})

describe('浏览器定位封装', () => {
  it('成功定位返回 WGS-84 整数 E6 坐标与 coordinateType', async () => {
    const getCurrentPosition = vi.fn((success, _failure, options) => {
      expect(options).toEqual({ enableHighAccuracy: true, timeout: 10000, maximumAge: 60000 })
      success({ coords: { latitude: 39.97768, longitude: 116.316417, accuracy: 18.5 } })
    })
    stubGeolocation({ getCurrentPosition })

    const position = await getCurrentLocation()
    expect(position).toEqual({
      latitude: 39.97768,
      longitude: 116.316417,
      latitudeE6: 39977680,
      longitudeE6: 116316417,
      coordinateType: 'wgs84',
      accuracyMeter: 18.5
    })
    expect(getCurrentPosition).toHaveBeenCalledTimes(1)
  })

  it('定位返回非法坐标时抛出 INVALID，不会下发错误的 E6 值', async () => {
    stubGeolocation({ getCurrentPosition: success => success({ coords: { latitude: Number.NaN, longitude: 116 } }) })
    const error = await getCurrentLocation().catch(caught => caught)
    expect(error).toBeInstanceOf(GeolocationError)
    expect(error.kind).toBe(GeolocationErrorKind.INVALID)
  })

  it('权限被拒绝（code=1）给出可手动兜底的中文提示', async () => {
    stubGeolocation({ getCurrentPosition: (_success, failure) => failure({ code: 1, message: 'User denied Geolocation' }) })
    const error = await getCurrentLocation().catch(caught => caught)
    expect(error).toBeInstanceOf(GeolocationError)
    expect(error.kind).toBe(GeolocationErrorKind.DENIED)
    expect(error.userMessage).toContain('定位权限被拒绝')
    expect(error.userMessage).toContain('预设位置或输入地址')
  })

  it('位置不可用（code=2）与超时（code=3）分别映射为不同错误', async () => {
    stubGeolocation({ getCurrentPosition: (_success, failure) => failure({ code: 2 }) })
    const unavailable = await getCurrentLocation().catch(caught => caught)
    expect(unavailable.kind).toBe(GeolocationErrorKind.UNAVAILABLE)
    expect(unavailable.userMessage).toContain('无法获取位置信息')

    stubGeolocation({ getCurrentPosition: (_success, failure) => failure({ code: 3 }) })
    const timeout = await getCurrentLocation().catch(caught => caught)
    expect(timeout.kind).toBe(GeolocationErrorKind.TIMEOUT)
    expect(timeout.userMessage).toContain('定位超时')
  })

  it('浏览器不支持定位 API 时给出 UNSUPPORTED', async () => {
    removeGeolocation()
    expect(isGeolocationSupported()).toBe(false)
    const error = await getCurrentLocation().catch(caught => caught)
    expect(error.kind).toBe(GeolocationErrorKind.UNSUPPORTED)
    expect(error.userMessage).toContain('不支持定位')
  })
})

describe('手动兜底位置', () => {
  it('预设位置均为 WGS-84 E6 度，可直接用于站点查询与路线规划', () => {
    expect(PRESET_LOCATIONS.length).toBeGreaterThanOrEqual(4)
    for (const preset of PRESET_LOCATIONS) {
      expect(Number.isInteger(preset.latitudeE6)).toBe(true)
      expect(Number.isInteger(preset.longitudeE6)).toBe(true)
      expect(preset.coordinateType).toBe('wgs84')
      expect(preset.label.length).toBeGreaterThan(0)
    }
    const beijing = presetLocation('beijing-zhongguancun')
    expect(beijing.latitudeE6).toBe(39977680)
    expect(beijing.coordinateType).toBe('wgs84')
  })

  it('未知预设位置抛出 INVALID', () => {
    expect(() => presetLocation('not-exist')).toThrow(GeolocationError)
  })
})
