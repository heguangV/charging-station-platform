import { api, unsupported } from './http'
import { flattenPage, mapOrderToFlow, mapUser, mapWalletEntry, legacyToOrderStatus, legacyToUserStatus } from './contract'

/**
 * 用户管理接口（Go 契约）。
 * 手机号不提供模糊扫描：列表仅 keyword/status 过滤，与隐私基线一致。
 */

/** 用户列表；参数 keyword、status(1/0)、page、pageSize。 */
export function fetchUsers(params = {}) {
  const { status, keyword, page, pageSize } = params
  return api
    .get('/admin/users', {
      keyword,
      status: status === undefined || status === null ? undefined : legacyToUserStatus(status),
      page,
      pageSize
    })
    .then(flattenPage)
    .then(data => ({ ...data, items: (Array.isArray(data.items) ? data.items : []).map(mapUser) }))
}

/** 用户详情：脱敏手机号、余额、注册时间；Go 契约无会话数/活动流程摘要。 */
export function fetchUser(userId) {
  return api.get(`/admin/users/${encodeURIComponent(userId)}`).then(mapUser)
}

/**
 * 冻结或解冻：Go 契约是两个 POST 端点（无 body），映射旧 status 0 冻结 / 1 解冻。
 * 乐观锁 version 在 Go 契约中不存在，参数保留但不再提交。
 */
export function setUserStatus(userId, { status }, { idempotencyKey } = {}) {
  const action = status === 0 ? 'freeze' : 'unfreeze'
  return api
    .post(`/admin/users/${encodeURIComponent(userId)}/${action}`, undefined, { idempotent: true, idempotencyKey })
    .then(mapUser)
}

/** 用户账务查询（A-04 第 7 步）：Go 契约 /admin/users/{userId}/transactions。 */
export function fetchUserTransactions(userId, { type, page, pageSize } = {}) {
  return api
    .get(`/admin/users/${encodeURIComponent(userId)}/transactions`, { type, page, pageSize })
    .then(flattenPage)
    .then(data => ({ ...data, items: (Array.isArray(data.items) ? data.items : []).map(mapWalletEntry) }))
}

/**
 * 用户订单历史（GET /admin/orders?userId=）：Go 契约的 AdminOrderFilter 已支持
 * userId 过滤。行形状与用户端订单列表一致（状态归一化 + 时间/电量换算）。
 */
export function fetchUserOrders(userId, { status, page, pageSize } = {}) {
  return api
    .get('/admin/orders', {
      userId,
      status: status === undefined || status === null ? undefined : legacyToOrderStatus(status),
      page,
      pageSize
    })
    .then(flattenPage)
    .then(data => ({ ...data, items: (Array.isArray(data.items) ? data.items : []).map(mapOrderToFlow) }))
}
