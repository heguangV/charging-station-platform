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

/**
 * 手工建档（POST /admin/users）：为用户名下的手机号建立账号。
 *
 * 账号出生即自注册形态：ACTIVE、零余额钱包、无密码——用户仍走短信登录，
 * 因此这里不接受任何凭据；操作员替用户选的密码是用户从未同意过的凭据。
 * 手机号已注册时服务端返回 409（ALREADY_EXISTS），不是可跳过的警告。
 */
export function createUser({ phone, displayName } = {}, { idempotencyKey } = {}) {
  return api.post(
    '/admin/users',
    { phone, displayName: displayName || undefined },
    { idempotent: true, idempotencyKey }
  )
}

/**
 * 批量建档（POST /admin/users/batch）：一次最多 1000 个账号，整批同事务。
 * 页内任一手机号已存在（含页内重复）则全部不创建，服务端逐条点名不会发生，
 * 因此页内查重在提交前由视图层完成。
 */
export function createUsersBatch({ users } = {}, { idempotencyKey } = {}) {
  return api.post(
    '/admin/users/batch',
    { users: (Array.isArray(users) ? users : []).map(user => ({
      phone: user.phone,
      displayName: user.displayName || undefined
    })) },
    { idempotent: true, idempotencyKey }
  )
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
