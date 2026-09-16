import { api, unsupported } from './http'
import { flattenPage, mapAuditEntry } from './contract'

/** 运维接口（Go 契约）：目前仅审计日志；备份能力暂缺。 */

/** 审计日志；Go 契约参数为 actorId、action、resourceType、resourceId + 分页（无时间范围）。 */
export function fetchAuditLogs(params = {}) {
  const { actorId, action, targetType, targetId, page, pageSize } = params
  return api
    .get('/admin/audit', {
      actorId,
      action,
      resourceType: targetType,
      resourceId: targetId,
      page,
      pageSize
    })
    .then(flattenPage)
    .then(data => ({ ...data, items: (Array.isArray(data.items) ? data.items : []).map(mapAuditEntry) }))
}

/** 备份记录：Go 契约暂无该域（待 B-06/部署线）。 */
export function fetchBackups() {
  return unsupported('备份记录在 Go 后端暂未提供')
}

/** 创建一致性备份：Go 契约暂无。 */
export function createBackup() {
  return unsupported('创建备份在 Go 后端暂未提供')
}

/** 隔离恢复验证：Go 契约暂无。 */
export function verifyBackup() {
  return unsupported('备份验证在 Go 后端暂未提供')
}
