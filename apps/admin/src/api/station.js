import { api, unsupported } from './http'
import { flattenPage } from './contract'

/** 站点接口（Go 契约）。所有写入均携带幂等键（16..128，服务端强制）。 */

/**
 * 站点列表；Go 契约仅支持 keyword + 分页（无 status/adcode 过滤，
 * 这两个筛选在 Go 契约补齐前不提交，避免伪装成已生效的筛选）。
 */
export function fetchStations(params = {}) {
  const { keyword, page, pageSize } = params
  return api.get('/admin/stations', { keyword, page, pageSize }).then(flattenPage)
}

/**
 * 新增站点：Go 契约字段为 code/name/address/latitudeE6/longitudeE6，
 * 不含 initialCharger（建设备暂无端点）、adcode、businessHours。
 */
export function createStation(payload, { idempotencyKey } = {}) {
  return api.post(
    '/admin/stations',
    {
      code: payload.code,
      name: payload.name,
      address: payload.address,
      latitudeE6: payload.latitudeE6,
      longitudeE6: payload.longitudeE6
    },
    { idempotent: true, idempotencyKey }
  )
}

/** 修改站点：Go 契约暂无该端点（待 B-01 决策）。 */
export function updateStation() {
  return unsupported('修改站点在 Go 后端暂未提供（待 B-01 契约决策）')
}

/**
 * 站点状态变更（PUT /admin/stations/{stationId}/status）：Go 契约接受
 * OPEN<->CLOSED、OPEN→DISABLED、CLOSED→DISABLED、DISABLED→OPEN。
 * 旧的启用/停用布尔语义映射为 OPEN/DISABLED。
 */
export function setStationEnabled(stationId, enabled, { reason, idempotencyKey } = {}) {
  return api
    .put(
      `/admin/stations/${encodeURIComponent(stationId)}/status`,
      { status: enabled ? 'OPEN' : 'DISABLED', reason },
      { idempotent: true, idempotencyKey }
    )
    .then(data => ({ ...data, enabled: data.status === 'OPEN' }))
}

/** 行政区基础价格版本：Go 契约的费率是设备级的（见 charger tariff），该域暂缺。 */
export function fetchTariffs() {
  return unsupported('基础价格版本在 Go 后端暂未提供（费率为设备级，见充电桩管理）')
}

export function createTariff() {
  return unsupported('创建价格版本在 Go 后端暂未提供')
}

export function createPriceAdjustment() {
  return unsupported('服务费调整在 Go 后端暂未提供')
}
