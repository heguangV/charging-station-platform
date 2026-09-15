import { api } from './http'

/** 管理员账号接口（接口文档 §6.8–§6.10）。列表与写入都需要 OWNER 权限。 */

/** §6.8 管理员账号列表；参数 page、pageSize。 */
export function fetchAccounts(params = {}) {
  return api.get('/admin/accounts', params)
}

/** §6.9 创建管理员账号：角色固定 OPERATOR，需要重新验证与幂等键。 */
export function createAccount({ username, password, reason }, { idempotencyKey } = {}) {
  return api.post('/admin/accounts', { username, password, reason }, { idempotent: true, idempotencyKey })
}

/** §6.10 停用或启用管理员；status 0 停用 / 1 启用，OWNER 不得停用本人。 */
export function setAccountStatus(adminId, { status, reason, version }, { idempotencyKey } = {}) {
  return api.put(
    `/admin/accounts/${encodeURIComponent(adminId)}/status`,
    { status, reason, version },
    { idempotent: true, idempotencyKey }
  )
}
