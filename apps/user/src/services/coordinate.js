/**
 * 纯函数坐标与单位换算工具（无任何依赖）。
 * 仓库约定：经纬度为整数 E6 度、金额为整数分、电量为整数毫瓦时、时间为 UTC Unix 秒；
 * 只有显示层才换算单位。
 */

const PI = Math.PI
const AXIS = 6378245.0 // 克拉索夫斯基椭球长半轴
const ECCENTRICITY = 0.00669342162296594323 // 椭球偏心率平方

/** outOfChina：中国大陆以外不偏移，直接使用原始 WGS-84 坐标。 */
export function outOfChina(latitude, longitude) {
  return longitude < 72.004 || longitude > 137.8347 || latitude < 0.8293 || latitude > 55.8271
}

function transformLatitude(x, y) {
  let ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * Math.sqrt(Math.abs(x))
  ret += ((20.0 * Math.sin(6.0 * x * PI) + 20.0 * Math.sin(2.0 * x * PI)) * 2.0) / 3.0
  ret += ((20.0 * Math.sin(y * PI) + 40.0 * Math.sin((y / 3.0) * PI)) * 2.0) / 3.0
  ret += ((160.0 * Math.sin((y / 12.0) * PI) + 320 * Math.sin((y * PI) / 30.0)) * 2.0) / 3.0
  return ret
}

function transformLongitude(x, y) {
  let ret = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * Math.sqrt(Math.abs(x))
  ret += ((20.0 * Math.sin(6.0 * x * PI) + 20.0 * Math.sin(2.0 * x * PI)) * 2.0) / 3.0
  ret += ((20.0 * Math.sin(x * PI) + 40.0 * Math.sin((x / 3.0) * PI)) * 2.0) / 3.0
  ret += ((150.0 * Math.sin((x / 12.0) * PI) + 300.0 * Math.sin((x / 30.0) * PI)) * 2.0) / 3.0
  return ret
}

function offset(latitude, longitude) {
  let deltaLatitude = transformLatitude(longitude - 105.0, latitude - 35.0)
  let deltaLongitude = transformLongitude(longitude - 105.0, latitude - 35.0)
  const radianLatitude = (latitude / 180.0) * PI
  let magic = Math.sin(radianLatitude)
  magic = 1 - ECCENTRICITY * magic * magic
  const sqrtMagic = Math.sqrt(magic)
  deltaLatitude = (deltaLatitude * 180.0) / (((AXIS * (1 - ECCENTRICITY)) / (magic * sqrtMagic)) * PI)
  deltaLongitude = (deltaLongitude * 180.0) / ((AXIS / sqrtMagic) * Math.cos(radianLatitude) * PI)
  return { deltaLatitude, deltaLongitude }
}

function validCoordinate(latitude, longitude) {
  return Number.isFinite(latitude) && Number.isFinite(longitude) && latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180
}

/** WGS-84（浏览器定位结果）转 GCJ-02（腾讯地图渲染坐标）。 */
export function wgs84ToGcj02(latitude, longitude) {
  if (!validCoordinate(latitude, longitude)) throw new RangeError('坐标不合法')
  if (outOfChina(latitude, longitude)) return { latitude, longitude }
  const { deltaLatitude, deltaLongitude } = offset(latitude, longitude)
  return { latitude: latitude + deltaLatitude, longitude: longitude + deltaLongitude }
}

/** GCJ-02 转 WGS-84：以偏移量做不动点迭代，往返误差小于 1e-7 度（约 1 厘米）。 */
export function gcj02ToWgs84(latitude, longitude) {
  if (!validCoordinate(latitude, longitude)) throw new RangeError('坐标不合法')
  if (outOfChina(latitude, longitude)) return { latitude, longitude }
  let wgsLatitude = latitude
  let wgsLongitude = longitude
  for (let iteration = 0; iteration < 4; iteration += 1) {
    const converted = wgs84ToGcj02(wgsLatitude, wgsLongitude)
    // 当前猜测值处的偏移量 delta(x) = wgs2gcj(x) - x，用 gcj - delta 逼近真实 WGS-84。
    wgsLatitude = latitude - (converted.latitude - wgsLatitude)
    wgsLongitude = longitude - (converted.longitude - wgsLongitude)
  }
  return { latitude: wgsLatitude, longitude: wgsLongitude }
}

/** 十进制度 -> 整数 E6 度。 */
export function toE6(decimal) {
  if (!Number.isFinite(decimal)) throw new RangeError('坐标不合法')
  return Math.round(decimal * 1e6)
}

/** 整数 E6 度 -> 十进制度（仅显示或地图渲染使用）。 */
export function fromE6(e6) {
  if (!Number.isFinite(e6)) throw new RangeError('坐标不合法')
  return e6 / 1e6
}

/** E6 坐标对 -> 腾讯地图渲染所需的 GCJ-02 十进制坐标对。 */
export function e6ToGcj02(latitudeE6, longitudeE6) {
  return wgs84ToGcj02(fromE6(latitudeE6), fromE6(longitudeE6))
}

/** 整数分 -> "1.35"（元），仅用于展示。 */
export function formatYuan(cent) {
  if (!Number.isFinite(cent)) return '--'
  return (cent / 100).toFixed(2)
}

/** 整数米 -> "230 米" / "2.3 km"。 */
export function formatDistance(distanceMeter) {
  if (!Number.isFinite(distanceMeter)) return '--'
  if (distanceMeter < 1000) return `${Math.round(distanceMeter)} 米`
  return `${(distanceMeter / 1000).toFixed(1)} km`
}

/** 整数毫瓦时 -> "12.35 kWh"。 */
export function formatEnergy(energyMwh) {
  if (!Number.isFinite(energyMwh)) return '--'
  return `${(energyMwh / 1e6).toFixed(2)} kWh`
}

/** UTC Unix 秒 -> 本地时间字符串；服务端只传秒，本地化在展示层完成。 */
export function formatDateTime(unixSecond) {
  if (!Number.isFinite(unixSecond) || unixSecond <= 0) return '--'
  return new Date(unixSecond * 1000).toLocaleString('zh-CN', { hour12: false })
}

/** 秒 -> "1 小时 12 分" / "8 分钟" / "45 秒"。 */
export function formatDuration(durationSecond) {
  if (!Number.isFinite(durationSecond) || durationSecond <= 0) return '--'
  if (durationSecond < 60) return `${Math.round(durationSecond)} 秒`
  if (durationSecond < 3600) return `${Math.round(durationSecond / 60)} 分钟`
  const hours = Math.floor(durationSecond / 3600)
  const minutes = Math.round((durationSecond % 3600) / 60)
  return minutes > 0 ? `${hours} 小时 ${minutes} 分` : `${hours} 小时`
}
