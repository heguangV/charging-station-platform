import { toE6 } from './coordinate'

/**
 * 浏览器标准 Geolocation API 封装。
 * 返回值一律是 WGS-84 整数 E6 度，并显式声明 coordinateType，交给服务端做坐标转换。
 */

export const GeolocationErrorKind = {
  UNSUPPORTED: 'UNSUPPORTED',
  DENIED: 'DENIED',
  UNAVAILABLE: 'UNAVAILABLE',
  TIMEOUT: 'TIMEOUT',
  INVALID: 'INVALID'
}

/** 携带可直接展示的中文提示的定位错误。 */
export class GeolocationError extends Error {
  constructor(kind, userMessage) {
    super(userMessage)
    this.name = 'GeolocationError'
    this.kind = kind
    this.userMessage = userMessage
  }
}

/** 手动兜底位置：定位被拒绝时仍可用地图与站点流程（坐标全部为 WGS-84）。 */
export const PRESET_LOCATIONS = [
  { id: 'beijing-zhongguancun', label: '北京 · 中关村', latitude: 39.97768, longitude: 116.316417 },
  { id: 'beijing-guomao', label: '北京 · 国贸', latitude: 39.90881, longitude: 116.461024 },
  { id: 'beijing-south-station', label: '北京 · 北京南站', latitude: 39.865385, longitude: 116.378646 },
  { id: 'shanghai-renmin', label: '上海 · 人民广场', latitude: 31.230416, longitude: 121.473701 },
  { id: 'guangzhou-tianhe', label: '广州 · 天河体育中心', latitude: 23.135509, longitude: 113.32402 },
  { id: 'shenzhen-civic', label: '深圳 · 市民中心', latitude: 22.543099, longitude: 114.057868 }
].map(preset => ({ ...preset, latitudeE6: toE6(preset.latitude), longitudeE6: toE6(preset.longitude), coordinateType: 'wgs84' }))

export function isGeolocationSupported() {
  return typeof navigator !== 'undefined' && !!navigator.geolocation
}

function failure(kind, userMessage) {
  return new GeolocationError(kind, userMessage)
}

function mapPositionError(error) {
  switch (error?.code) {
    case 1:
      return failure(GeolocationErrorKind.DENIED, '定位权限被拒绝，请选择预设位置或输入地址后继续。')
    case 2:
      return failure(GeolocationErrorKind.UNAVAILABLE, '暂时无法获取位置信息，请移动到开阔处或手动选择位置。')
    case 3:
      return failure(GeolocationErrorKind.TIMEOUT, '定位超时，请重试或手动选择位置。')
    default:
      return failure(GeolocationErrorKind.UNAVAILABLE, '定位失败，请重试或手动选择位置。')
  }
}

/**
 * 获取当前位置。
 * @returns {Promise<{latitudeE6:number, longitudeE6:number, coordinateType:'wgs84', latitude:number, longitude:number, accuracyMeter:number}>}
 */
export function getCurrentLocation({ enableHighAccuracy = true, timeout = 10000, maximumAge = 60000 } = {}) {
  if (!isGeolocationSupported()) {
    return Promise.reject(failure(GeolocationErrorKind.UNSUPPORTED, '当前浏览器不支持定位，请手动选择位置或输入地址。'))
  }
  return new Promise((resolve, reject) => {
    navigator.geolocation.getCurrentPosition(
      position => {
        const { latitude, longitude, accuracy } = position.coords || {}
        if (!Number.isFinite(latitude) || !Number.isFinite(longitude)) {
          reject(failure(GeolocationErrorKind.INVALID, '定位返回的坐标无效，请重试或手动选择位置。'))
          return
        }
        resolve({
          latitude,
          longitude,
          latitudeE6: toE6(latitude),
          longitudeE6: toE6(longitude),
          coordinateType: 'wgs84',
          accuracyMeter: Number.isFinite(accuracy) ? accuracy : null
        })
      },
      error => reject(mapPositionError(error)),
      { enableHighAccuracy, timeout, maximumAge }
    )
  })
}

/** 预设位置转为与真实定位一致的返回结构。 */
export function presetLocation(presetId) {
  const preset = PRESET_LOCATIONS.find(item => item.id === presetId)
  if (!preset) throw new GeolocationError(GeolocationErrorKind.INVALID, '预设位置不存在')
  return {
    latitude: preset.latitude,
    longitude: preset.longitude,
    latitudeE6: preset.latitudeE6,
    longitudeE6: preset.longitudeE6,
    coordinateType: 'wgs84',
    accuracyMeter: null,
    label: preset.label
  }
}
