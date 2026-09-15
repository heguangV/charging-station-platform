import { api } from './http'

/** 管理员认证接口（接口文档 §6.1、§6.2、§6.3、§6.11）。 */

/** §6.1 管理员登录；deviceId 用于会话审计，重复登录不产生新的设备维度。 */
export function login({ username, password, deviceId = 'ncs-admin-web' }) {
  return api.post('/admin/auth/login', { username, password, deviceId })
}

/** §6.2 敏感操作重新验证，成功后 15 分钟内免再次验证。 */
export function reauth(password) {
  return api.post('/admin/auth/reauth', { password })
}

/** §6.3 管理员退出；重复退出幂等成功，因此不带幂等键。 */
export function logout() {
  return api.post('/admin/auth/logout')
}

/** §6.11 修改本人密码：成功后清除 mustChangePassword，其他终端会话失效。 */
export function changeOwnPassword({ currentPassword, newPassword }) {
  return api.put('/admin/me/password', { currentPassword, newPassword })
}
