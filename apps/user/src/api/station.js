import { api } from './http'

/** 站点、设备与价格接口（接口文档 §4）。文件里的经纬度均为整数 E6 度。 */

/**
 * §4.1 附近站点。keyword 是待地理编码的当前位置地址，不是站名过滤条件；
 * 经纬度必须成对出现，否则只依赖 keyword。
 */
export function fetchStations({ latitudeE6, longitudeE6, keyword, chargerType, page, pageSize } = {}) {
  const hasCoordinate = Number.isInteger(latitudeE6) && Number.isInteger(longitudeE6)
  return api.get('/user/stations', {
    latitudeE6: hasCoordinate ? latitudeE6 : undefined,
    longitudeE6: hasCoordinate ? longitudeE6 : undefined,
    keyword,
    chargerType,
    page,
    pageSize
  })
}

/** §4.2 站点详情：营业时间、基础价格、实时设备统计与支持的充电类型。 */
export function fetchStation(stationId) {
  return api.get(`/user/stations/${encodeURIComponent(stationId)}`)
}

/** §4.3 设备列表。 */
export function fetchChargers(stationId, { chargerType, status, page, pageSize } = {}) {
  return api.get(`/user/stations/${encodeURIComponent(stationId)}/chargers`, { chargerType, status, page, pageSize })
}

/** §4.4 当前预估价格（仅供展示，不作为结算价格承诺）。 */
export function fetchQuote(stationId, { chargerType } = {}) {
  return api.get(`/user/stations/${encodeURIComponent(stationId)}/quote`, { chargerType })
}

/**
 * §4.5 路线规划。此处坐标与 keyword 均表示起点，禁止填入目标电站坐标。
 * 系统定位得到的 WGS-84 坐标必须成对提交并声明 coordinateType=wgs84。
 */
export function fetchRoute(stationId, { latitudeE6, longitudeE6, keyword, mode = 'driving', coordinateType } = {}) {
  const hasCoordinate = Number.isInteger(latitudeE6) && Number.isInteger(longitudeE6)
  return api.get(`/user/stations/${encodeURIComponent(stationId)}/route`, {
    latitudeE6: hasCoordinate ? latitudeE6 : undefined,
    longitudeE6: hasCoordinate ? longitudeE6 : undefined,
    keyword: hasCoordinate ? undefined : keyword,
    mode,
    coordinateType: hasCoordinate ? coordinateType || 'wgs84' : undefined
  })
}

/** §5.10 场站评论墙（只读，作者已由服务端脱敏）。 */
export function fetchStationReviews(stationId) {
  return api.get(`/user/stations/${encodeURIComponent(stationId)}/reviews`)
}
