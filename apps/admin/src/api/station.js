import { api } from './http'

/** 站点与价格接口（接口文档 §7.1–§7.5、§7.11–§7.13）。所有写入均携带幂等键。 */

/** §7.1 站点列表；参数 status、adcode、keyword、page、pageSize。 */
export function fetchStations(params = {}) {
  return api.get('/admin/stations', params)
}

/** §7.2 新增站点：initialCharger 必填，站点与初始设备在同一事务内创建。 */
export function createStation(payload, { idempotencyKey } = {}) {
  return api.post('/admin/stations', payload, { idempotent: true, idempotencyKey })
}

/** §7.3 修改站点：只允许 name/address/adcode/latitudeE6/longitudeE6/businessHours/version。 */
export function updateStation(stationId, patch, { idempotencyKey } = {}) {
  return api.put(`/admin/stations/${encodeURIComponent(stationId)}`, patch, {
    idempotent: true,
    idempotencyKey
  })
}

/** §7.4/§7.5 停用或启用站点：需要原因与当前 version。 */
export function setStationEnabled(stationId, enabled, { reason, version, idempotencyKey } = {}) {
  const action = enabled ? 'enable' : 'disable'
  return api.post(
    `/admin/stations/${encodeURIComponent(stationId)}/${action}`,
    { reason, version },
    { idempotent: true, idempotencyKey }
  )
}

/** §7.11 基础价格版本；参数 adcode、effectiveAt、page、pageSize。 */
export function fetchTariffs(params = {}) {
  return api.get('/admin/tariffs', params)
}

/** §7.12 创建基础价格版本；同一行政区有效期不得重叠。 */
export function createTariff(payload, { idempotencyKey } = {}) {
  return api.post('/admin/tariffs', payload, { idempotent: true, idempotencyKey })
}

/** §7.13 批准服务费调整；adjustmentBp 为 -2000~2000、步长 500。 */
export function createPriceAdjustment(payload, { idempotencyKey } = {}) {
  return api.post('/admin/price-adjustments', payload, { idempotent: true, idempotencyKey })
}
