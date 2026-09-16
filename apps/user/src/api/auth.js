import { api } from './http'
import { composeProfile, mapIdentity, mapProfileView } from './contract'

/**
 * 用户认证与资料接口（Go 契约）。手机号与验证码只提交给服务端，不在前端留存。
 *
 * 旧 /user/me 合并资料视图在 Go 契约中拆分为 /me（身份）、/me/profile（资料）与
 * /wallet（余额），前端在 fetchProfile 内聚合成既有形状。
 */

/** 获取验证码；Go 契约没有 purpose 参数（验证码登录自动注册）。 */
export function sendSmsCode(phone) {
  return api.post('/auth/user/sms/code', { phone }).then(data => ({
    ...data,
    // Go 在模拟短信（开发环境）响应里带 code 字段，对应旧契约的 developmentCode。
    developmentCode: typeof data.code === 'string' ? data.code : '',
    retryAfterSec: null
  }))
}

/** 验证码登录（手机号不存在时服务端自动注册）。 */
export function loginWithSms({ phone, smsCode }) {
  return api.post('/auth/user/login/sms', { phone, code: smsCode }).then(mapLoginSession)
}

/** 用户名或手机号 + 密码登录（Go 契约字段为 account）。 */
export function loginWithPassword({ loginName, password }) {
  return api.post('/auth/user/login', { account: loginName, password }).then(mapLoginSession)
}

/**
 * 用户名密码注册（Go 契约 201 返回登录会话）：phone/password/smsCode 必填，
 * username 可选（缺省时以手机号命名）。验证码用同一短信端点获取。
 */
export function registerAccount({ username, phone, password, smsCode }) {
  return api
    .post('/auth/user/register', {
      phone,
      password,
      smsCode,
      ...(username ? { username } : {})
    })
    .then(mapLoginSession)
}

/** 退出当前会话；重复退出幂等成功，因此不需要幂等键。 */
export function logout() {
  return api.post('/auth/logout')
}

/** 聚合资料：/me + /me/profile + /wallet → 旧合并视图形状。 */
export function fetchProfile() {
  return Promise.all([api.get('/me'), api.get('/me/profile'), api.get('/wallet')]).then(
    ([identity, profile, wallet]) => composeProfile(identity, profile, wallet)
  )
}

/**
 * 修改资料（Go 契约 ProfileUpdateRequest：displayName / avatarUrl 至少一项，
 * 无版本乐观锁）。头像为 URL 字段：Go 契约不提供文件上传，前端以 URL 方式设置。
 */
export function updateProfile(patch) {
  return api.put('/me/profile', patch).then(data => ({ user: mapProfileView(data) }))
}

/** 修改昵称。 */
export function updateNickname(nickname) {
  return updateProfile({ displayName: nickname })
}

/** 修改头像地址（Go 契约 avatarUrl 字符串，≤512，空串表示无头像）。 */
export function updateAvatarUrl(avatarUrl) {
  return updateProfile({ avatarUrl })
}

function mapLoginSession(session) {
  return { ...session, user: mapIdentity(session?.identity) }
}
