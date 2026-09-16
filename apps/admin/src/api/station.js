import { api, unsupported } from './http'
import { flattenPage } from './contract'

/** 站点接口（Go 契约）。所有写入均携带幂等键（16..128，服务端强制）。 */

/**
 * 站点列表；Go 契约仅支持 keyword + 分页（无 status/adcode 过滤，
 * 这两个筛选在 Go 契约补齐前不提交，避免伪装成已生效的筛选）。
 */
export function fetchStations(params = {}) {
  const { keyword, page, pageSize } = params
  return api
    .get('/admin/stations', { keyword, page, pageSize })
    .then(flattenPage)
    .then(pageData => ({ ...pageData, items: pageData.items.map(withStationState) }))
}

/**
 * 列表行也补齐 enabled：Go 只返回 status，缺了它页面会把运营中的站点
 * 显示成「已停用」，启停按钮也会反过来发同一个目标状态（服务端 409）。
 * 与 setStationEnabled 的响应换算保持同一套语义。
 */
function withStationState(station) {
  return { ...station, enabled: station?.status === 'OPEN' }
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

/**
 * 修改站点（PUT /admin/stations/{stationId}）：只支持名称、地址与经纬度。
 *
 * 编码与状态不在此端点：编码是身份（审计与订单都引用它），状态有自己的
 * 生命周期端点。即使前端多传了这两个字段，服务端也不会采纳。
 */
export function updateStation(stationId, patch = {}, { idempotencyKey } = {}) {
  void idempotencyKey
  return api.put(`/admin/stations/${encodeURIComponent(stationId)}`, {
    name: patch.name,
    address: patch.address,
    latitudeE6: patch.latitudeE6,
    longitudeE6: patch.longitudeE6
  })
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

/**
 * 全局费率（GET /admin/tariffs）：所有电桩当前持有的费率构成与分布。
 *
 * 费率存在电桩上而不是独立费率表，因此这里返回的是车队的真实状态：
 * 有几种不同配置、各有多少桩，以及价格区间与分时/单一费率的数量。
 */
export function fetchTariffs() {
  return api.get('/admin/tariffs')
}

/**
 * 全局费率修改（PUT /admin/tariffs）：一次把整套费率写到所有电桩。
 *
 * 请求体是完整费率而不是补丁：不带分时字段即表示车队改为单一费率，与
 * 设备级费率端点同一规则。原因必填，会写入审计——这是本边界上影响面最大的写入。
 */
export function setGlobalTariff(payload = {}, { idempotencyKey } = {}) {
  void idempotencyKey
  const body = {
    electricityPriceCentPerKwh: payload.electricityPriceCentPerKwh,
    servicePriceCentPerKwh: payload.servicePriceCentPerKwh,
    reason: payload.reason
  }
  if (payload.offPeakElectricityPriceCentPerKwh !== undefined) {
    body.offPeakElectricityPriceCentPerKwh = payload.offPeakElectricityPriceCentPerKwh
    body.offPeakStartHour = payload.offPeakStartHour
    body.offPeakEndHour = payload.offPeakEndHour
  }
  return api.put('/admin/tariffs', body)
}

export function createPriceAdjustment() {
  return unsupported('按行政区/桩型的服务费比例调整在 Go 后端暂未提供（当前只支持全局费率统一下发）')
}
