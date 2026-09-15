import { api } from './http'

/** 设备接口（接口文档 §7.6–§7.10）。所有写入均携带幂等键。 */

/** §7.6 设备列表；参数 stationId、status、chargerType、keyword、page、pageSize。 */
export function fetchChargers(params = {}) {
  return api.get('/admin/chargers', params)
}

/** §7.7 批量创建设备：一次最多 100 台，全部成功或全部回滚。 */
export function createChargersBatch({ stationId, chargers }, { idempotencyKey } = {}) {
  return api.post('/admin/chargers/batch', { stationId, chargers }, { idempotent: true, idempotencyKey })
}

/** §7.8 设置设备状态：targetStatus、reason 与当前 version 必填。 */
export function setChargerStatus(chargerId, { targetStatus, reason, version }, { idempotencyKey } = {}) {
  return api.put(
    `/admin/chargers/${encodeURIComponent(chargerId)}/status`,
    { targetStatus, reason, version },
    { idempotent: true, idempotencyKey }
  )
}

/** §7.9 远程重启：必须二次确认（confirm=true）并填写原因，返回 202 与命令编号。 */
export function createRestartCommand(chargerId, { reason }, { idempotencyKey } = {}) {
  return api.post(
    `/admin/chargers/${encodeURIComponent(chargerId)}/restart-commands`,
    { confirm: true, reason },
    { idempotent: true, idempotencyKey }
  )
}

/** §7.10 查询设备命令状态：PENDING/RUNNING/SUCCEEDED/FAILED。 */
export function fetchDeviceCommand(commandNo) {
  return api.get(`/admin/device-commands/${encodeURIComponent(commandNo)}`)
}
