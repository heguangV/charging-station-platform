import { api, unsupported } from './http'
import { flattenPage, legacyToChargerStatus, legacyToConnectorType, mapCharger } from './contract'

/** 设备接口（Go 契约）。写入均携带幂等键（16..128，服务端强制）。 */

/**
 * 设备列表；Go 契约参数为 stationId、status、page、pageSize（无 keyword/chargerType）。
 * keyword/chargerType 过滤在 Go 契约补齐前不提交，避免伪装成已生效的筛选。
 */
export function fetchChargers(params = {}) {
  const { stationId, status, page, pageSize } = params
  return api
    .get('/admin/chargers', {
      stationId,
      status: status === undefined || status === null ? undefined : legacyToChargerStatus(status),
      page,
      pageSize
    })
    .then(flattenPage)
    .then(data => ({ ...data, items: (Array.isArray(data.items) ? data.items : []).map(mapCharger) }))
}

/**
 * 远程重启：Go 返回 202 + {commandId, status}。commandId 是
 * charger_command_outcomes.command_id 的业务标识（非订单号）。
 */
export function createRestartCommand(chargerId, { reason }, { idempotencyKey } = {}) {
  return api.post(
    `/admin/chargers/${encodeURIComponent(chargerId)}/restart`,
    { reason },
    { idempotent: true, idempotencyKey }
  )
}

/**
 * 命令状态查询（GET /admin/device-commands/{commandId}）：返回平台为该命令记录的
 * 网关回执结果（result: COMPLETED/FAILED + applied）。回执未到达时 Go 返回 404，
 * 调用方按"仍在等待回执"处理。
 */
export function fetchDeviceCommand(commandId) {
  return api.get(`/admin/device-commands/${encodeURIComponent(commandId)}`)
}

/**
 * 直接设置设备状态（PUT /admin/chargers/{chargerId}/status）：Go 契约只接受
 * IDLE/DISABLED 两个目标状态（IDLE<->DISABLED、FAULT→IDLE/DISABLED；
 * OCCUPIED/RESTARTING 一律 409）。旧数字码 0→IDLE、3→DISABLED。
 */
export function setChargerStatus(chargerId, { targetStatus, reason }, { idempotencyKey } = {}) {
  const status = targetStatus === 0 ? 'IDLE' : targetStatus === 3 ? 'DISABLED' : null
  if (!status) {
    return unsupported('Go 契约只允许把设备置为空闲（IDLE）或已停用（DISABLED）')
  }
  return api
    .put(
      `/admin/chargers/${encodeURIComponent(chargerId)}/status`,
      { status, reason },
      { idempotent: true, idempotencyKey }
    )
    .then(data => ({ ...data, status: data.status === 'IDLE' ? 0 : 3 }))
}

/** 批量创建设备：Go 契约暂无该端点（待 B-01 决策）。 */
/**
 * 批量建桩（POST /admin/chargers/batch）：一次 1~100 台，整批同事务。
 *
 * 请求体只带操作员输入的字段：编号、连接器类型与功率。状态、价格与版本由服务端决定，
 * 前端不提交；旧表单里的接口标准在 Go 契约中没有对应列，因此不发送。
 * 连接器类型在这里从旧数字桩型翻译为契约用词 AC/DC。
 */
export function createChargersBatch({ stationId, chargers } = {}, { idempotencyKey } = {}) {
  return api.post(
    '/admin/chargers/batch',
    {
      stationId,
      chargers: (Array.isArray(chargers) ? chargers : []).map(charger => ({
        code: charger.code,
        connectorType: legacyToConnectorType(charger.chargerType),
        powerWatt: charger.powerWatt
      }))
    },
    { idempotent: true, idempotencyKey }
  )
}
