import { api } from './http'

/**
 * 用户管理接口（接口文档 §6.4–§6.7）。
 * 手机号只支持完整精确匹配或后四位匹配，不提供任意模糊扫描。
 */

/** §6.4 用户列表；参数 status、phoneExact、phoneLast4、page、pageSize、sort。 */
export function fetchUsers(params = {}) {
  return api.get('/admin/users', params)
}

/** §6.5 用户详情：脱敏手机号、钱包汇总、会话数量与活动流程摘要。 */
export function fetchUser(userId) {
  return api.get(`/admin/users/${encodeURIComponent(userId)}`)
}

/** §6.6 冻结或解冻：status 0 冻结 / 1 解冻，需要原因与当前 version。 */
export function setUserStatus(userId, { status, reason, version }, { idempotencyKey } = {}) {
  return api.put(
    `/admin/users/${encodeURIComponent(userId)}/status`,
    { status, reason, version },
    { idempotent: true, idempotencyKey }
  )
}

/** §6.7 用户订单历史；status 只允许 60/70/90，管理员访问会写审计日志。 */
export function fetchUserOrders(userId, params = {}) {
  return api.get(`/admin/users/${encodeURIComponent(userId)}/orders`, params)
}
