import { api } from './http'
import {
  connectorTypeToLegacy,
  flattenPage,
  legacyChargerTypeToConnector,
  mapChargerStatus,
  mapChargerStatusText,
  mapWallEntry
} from './contract'

/**
 * 站点与设备接口（Go 契约）。经纬度均为整数 E6 度。
 *
 * 与旧 C++ 契约的差异：
 * - 站点/设备路径不带 /user 前缀，设备列表是 /chargers?stationId= 而非子路径；
 * - 桩型字段为 connectorType（AC/DC），前端内部继续用 0/1；
 * - 没有独立的 /quote 与 /route 端点：单价取站点详情的 minPriceCentPerKwh，
 *   路线规划等待 A-07，前端不做降级伪装。
 */

/** 附近站点。经纬度必须成对出现，否则只提交 keyword。 */
export function fetchStations({ latitudeE6, longitudeE6, keyword, chargerType, page, pageSize } = {}) {
  const hasCoordinate = Number.isInteger(latitudeE6) && Number.isInteger(longitudeE6)
  return api
    .get('/stations', {
      latitudeE6: hasCoordinate ? latitudeE6 : undefined,
      longitudeE6: hasCoordinate ? longitudeE6 : undefined,
      keyword,
      connectorType: chargerType === undefined ? undefined : legacyChargerTypeToConnector(chargerType),
      page,
      pageSize
    })
    .then(flattenPage)
    .then(data => ({
      ...data,
      // Go returns chargerCount/idleChargerCount/minPriceCentPerKwh;
      // the existing card consumes the legacy display names below.
      items: (Array.isArray(data.items) ? data.items : []).map(mapStation),
      locationFallback: false
    }))
}

/**
 * Go 站点 DTO → 既有卡片消费的展示字段。
 *
 * 站点列表与 AI 助手的推荐结果共用这一份映射：两处各写一份就会出现同一站点
 * 在列表里显示一个价格、在助手卡片里显示另一个的局面。
 */
export function mapStation(raw) {
  if (!raw || typeof raw !== 'object') return null
  return {
    ...raw,
    operationalCount: raw.idleChargerCount,
    idleCount: raw.idleChargerCount,
    totalCount: raw.chargerCount,
    totalPriceCentPerKwh: raw.minPriceCentPerKwh
  }
}

/** 站点详情：Go 契约无营业时间（展示层回退“全天”）与可用/总数拆分字段，这里补齐。 */
export function fetchStation(stationId) {
  return api.get(`/stations/${encodeURIComponent(stationId)}`).then(detail => ({
    ...detail,
    openingHours: detail.openingHours || '',
    operationalCount: Number.isFinite(detail.idleChargerCount) ? detail.idleChargerCount : detail.chargerCount,
    totalCount: detail.chargerCount
  }))
}

/** 设备列表：stationId 是查询参数（非子路径）。 */
export function fetchChargers(stationId, { chargerType, status, page, pageSize } = {}) {
  return api
    .get('/chargers', {
      stationId,
      connectorType: chargerType === undefined || chargerType === null ? undefined : legacyChargerTypeToConnector(chargerType),
      status,
      page,
      pageSize
    })
    .then(flattenPage)
    .then(data => ({ ...data, items: (Array.isArray(data.items) ? data.items : []).map(mapCharger) }))
}

/** 场站评论墙（只读，作者已由服务端脱敏）。 */
export function fetchStationReviews(stationId, { page, pageSize } = {}) {
  return api
    .get(`/stations/${encodeURIComponent(stationId)}/reviews`, { page, pageSize })
    .then(flattenPage)
    .then(data => ({ ...data, items: (Array.isArray(data.items) ? data.items : []).map(mapWallEntry) }))
}

function mapCharger(raw) {
  if (!raw || typeof raw !== 'object') return null
  return {
    ...raw,
    chargerType: connectorTypeToLegacy(raw.type),
    status: mapChargerStatus(raw.status),
    statusText: mapChargerStatusText(raw.status),
    connectorStandard: ''
  }
}
