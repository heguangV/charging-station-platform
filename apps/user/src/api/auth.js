import { API_BASE, api, getAccessToken, randomId } from './http'

/** 用户认证与资料接口（接口文档 §2）。手机号与验证码只提交给服务端，不在前端留存。 */

/** §2.1 获取模拟验证码；purpose 取 LOGIN / REGISTER / RESET_PASSWORD。 */
export function sendSmsCode(phone, purpose = 'LOGIN') {
  return api.post('/user/auth/sms/code', { phone, purpose })
}

/** §2.4 验证码登录（手机号不存在时服务端自动注册）。 */
export function loginWithSms({ phone, smsCode, deviceId = 'web-driver' }) {
  return api.post('/user/auth/login/sms', { phone, smsCode, deviceId })
}

/** §2.3 用户名或手机号 + 密码登录。 */
export function loginWithPassword({ loginName, password, deviceId = 'web-driver' }) {
  return api.post('/user/auth/login/password', { loginName, password, deviceId })
}

/** §2.2 用户名密码注册。 */
export function registerAccount({ username, phone, password, smsCode, deviceId = 'web-driver' }) {
  return api.post('/user/auth/register', { username, phone, password, smsCode, deviceId })
}

/** §2.5 退出当前会话；重复退出返回成功，因此不需要幂等键。 */
export function logout() {
  return api.post('/user/auth/logout')
}

/** §2.8 个人资料（含余额、欠费与 hasActiveFlow、version）。 */
export function fetchProfile() {
  return api.get('/user/me')
}

/** §2.9 修改昵称，必须提交当前 version。 */
export function updateNickname(nickname, version) {
  return api.put('/user/me', { nickname, version })
}

/** §2.10 上传头像（multipart/form-data，字段名 file）。 */
export function uploadAvatar(file) {
  const form = new FormData()
  form.append('file', file)
  return api.post('/user/me/avatar', form, { idempotent: true })
}

/** §2.11 当前头像接口地址（服务端要求 Bearer 令牌，不能直接作为 <img src> 使用）。 */
export function avatarContentUrl() {
  return `${API_BASE}/user/me/avatar/content`
}

/**
 * §2.11 读取当前头像内容。接口需要 Bearer 令牌，浏览器 <img> 不会带上 Authorization，
 * 因此这里显式请求后转成 blob URL；未设置头像（404）时返回空串，由调用方使用占位图。
 */
export async function fetchAvatarObjectUrl() {
  try {
    const headers = { 'X-Request-ID': randomId() }
    const token = getAccessToken()
    if (token) headers.Authorization = `Bearer ${token}`
    const response = await fetch(avatarContentUrl(), { headers, cache: 'no-store' })
    if (!response.ok) return ''
    const blob = await response.blob()
    return typeof URL !== 'undefined' && typeof URL.createObjectURL === 'function' ? URL.createObjectURL(blob) : ''
  } catch {
    return ''
  }
}
