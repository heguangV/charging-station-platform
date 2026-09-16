import { api, unsupported } from './http'
import { mapAdminIdentity } from './contract'

/** 管理员认证接口（Go 契约）。 */

/** 管理员登录；Go 契约不收 deviceId，身份里带 adminRole（SUPER_ADMIN/OPERATOR/AUDITOR）。 */
export function login({ account, password }) {
  // 字段名是契约的一部分：Go 的 LoginRequest 要求 `account`，发送 `username` 会被判为
  // 参数越界并返回 400 invalid_argument，登录永远失败。这里的入参沿用契约用词，
  // 避免调用方以为可以传任意用户名字段。
  return api.post('/auth/admin/login', { account, password }).then(session => ({
    ...session,
    admin: mapAdminIdentity(session?.identity)
  }))
}

/**
 * 重新验证（旧 §6.2）与修改本人密码（旧 §6.11）在 Go 契约中暂缺：
 * runWithReauth 由服务端 REAUTH_REQUIRED(23) 驱动，Go 永不返回该码，机制自然休眠；
 * 这里保留显式的不可用语义，防止未来误用打在真实 404 上。
 */
export function reauth() {
  return unsupported('重新验证在 Go 后端暂未提供（待 B-01 契约决策）')
}

export function changeOwnPassword() {
  return unsupported('修改本人密码在 Go 后端暂未提供（待 B-01 契约决策）')
}

/** 管理员退出；重复退出幂等成功，Go 为用户/管理员统一端点。 */
export function logout() {
  return api.post('/auth/logout')
}
