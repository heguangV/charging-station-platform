import { api } from './http'

/** 运维接口：审计日志与备份（接口文档 §8.5–§8.8）。审计与备份都需要 OWNER 权限。 */

/** §8.5 审计日志；参数 actorId、action、targetType、targetId、fromAt、toAt、page、pageSize。 */
export function fetchAuditLogs(params = {}) {
  return api.get('/admin/audit-logs', params)
}

/** §8.7 备份记录：只返回编号、校验和、大小、状态与时间，不含可读文件路径。 */
export function fetchBackups() {
  return api.get('/admin/backups')
}

/** §8.6 创建一致性备份：需要重新验证与幂等键，返回 202 与 backupNo。 */
export function createBackup({ idempotencyKey } = {}) {
  return api.post('/admin/backups', {}, { idempotent: true, idempotencyKey })
}

/** §8.8 隔离恢复验证：使用独立临时路径，禁止覆盖当前数据库。 */
export function verifyBackup(backupNo, { idempotencyKey } = {}) {
  return api.post(
    `/admin/backups/${encodeURIComponent(backupNo)}/verifications`,
    {},
    { idempotent: true, idempotencyKey }
  )
}
