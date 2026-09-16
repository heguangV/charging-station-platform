import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useAuthStore, LOGIN_LOCK_SECONDS } from '../src/stores/auth'
import { useUsersStore } from '../src/stores/users'
import { TOKEN_STORAGE_KEY, clearAccessToken, getAccessToken } from '../src/api/http'
import { failResponse, flush, installFetch, okResponse } from './helpers'

/** 每个用例自行安装的 fetch 替身（用于断言请求次数与请求头）。 */
let harness = null

/** Go 契约登录响应：身份携带 adminRole（SUPER_ADMIN/OPERATOR/AUDITOR），无 OWNER。 */
const GO_IDENTITY = {
  id: 1,
  role: 'ADMIN',
  adminRole: 'SUPER_ADMIN',
  displayName: 'admin',
  status: 'ACTIVE'
}

function loginEnvelope(identity = GO_IDENTITY) {
  return okResponse({ accessToken: 'admin-token-1', expiresAt: 1893456000, identity })
}

let auth = null

beforeEach(() => {
  setActivePinia(createPinia())
  sessionStorage.clear()
  localStorage.clear()
  clearAccessToken()
  auth = useAuthStore()
  auth.setReauthProvider(null)
})

afterEach(() => {
  auth.clearLock()
  auth.setReauthProvider(null)
  vi.unstubAllGlobals()
  clearAccessToken()
  sessionStorage.clear()
  localStorage.clear()
})

describe('登录与令牌存储', () => {
  it('登录成功后令牌只写 sessionStorage，localStorage 完全不被触碰', async () => {
    harness = installFetch([loginEnvelope()])
    await expect(auth.login({ account: 'admin', password: 'example-password' })).resolves.toBe(true)

    expect(auth.token).toBe('admin-token-1')
    expect(getAccessToken()).toBe('admin-token-1')
    expect(sessionStorage.getItem(TOKEN_STORAGE_KEY)).toBe('admin-token-1')
    expect(localStorage.length).toBe(0)
    expect(localStorage.getItem(TOKEN_STORAGE_KEY)).toBeNull()
    expect(auth.isLoggedIn).toBe(true)
    // Go 契约的身份 displayName 即管理员用户名。
    expect(auth.displayName).toBe('admin')
    // Go 契约没有 OWNER 角色：SUPER_ADMIN/OPERATOR/AUDITOR，isOwner 恒为 false。
    expect(auth.isOwner).toBe(false)
  })

  it('登录请求体使用 Go 契约字段 account（发送 username 会被判为 400）', async () => {
    harness = installFetch([loginEnvelope()])
    await auth.login({ account: 'ops_wang', password: 'ncs-Initial-2026' })
    expect(harness.bodyOf(0)).toEqual({
      account: 'ops_wang',
      password: 'ncs-Initial-2026'
    })
    expect(harness.urlOf(0)).toBe('/api/v1/auth/admin/login')
  })

  it('刷新页面后可从 sessionStorage 恢复令牌与资料快照', async () => {
    harness = installFetch([loginEnvelope()])
    await auth.login({ account: 'admin', password: 'example-password' })

    const restored = useAuthStore()
    restored.$reset()
    restored.token = null
    restored.admin = null

    expect(restored.restore()).toBe(true)
    expect(restored.isLoggedIn).toBe(true)
    expect(restored.displayName).toBe('admin')
    expect(restored.roles).toEqual(['SUPER_ADMIN'])
  })
})

describe('退出与改密', () => {
  it('退出清理令牌、资料与 sessionStorage（Go 为统一注销端点）', async () => {
    harness = installFetch([loginEnvelope(), okResponse({})])
    await auth.login({ account: 'admin', password: 'example-password' })
    await auth.logout()

    expect(auth.token).toBeNull()
    expect(auth.admin).toBeNull()
    expect(auth.notice).toBe('已退出登录')
    expect(sessionStorage.getItem(TOKEN_STORAGE_KEY)).toBeNull()
    expect(harness.urlOf(1)).toBe('/api/v1/auth/logout')
  })

  it('退出接口失败也要清理本地会话', async () => {
    harness = installFetch([loginEnvelope(), failResponse({ status: 500, code: 13, userMessage: '服务异常' })])
    await auth.login({ account: 'admin', password: 'example-password' })
    await auth.logout()
    expect(auth.token).toBeNull()
    expect(getAccessToken()).toBeNull()
  })

  it('Go 契约暂无改密端点：mustChangePassword 恒为 false，改密调用显式失败', async () => {
    harness = installFetch([loginEnvelope()])
    await auth.login({ account: 'ops_wang', password: 'ncs-Initial-2026' })
    expect(auth.mustChangePassword).toBe(false)

    await expect(
      auth.changeOwnPassword({ currentPassword: 'ncs-Initial-2026', newPassword: 'ncs-New-2026-pw' })
    ).rejects.toThrow('暂未提供')
    expect(harness.count()).toBe(1)
  })
})

describe('登录锁定（连续失败 5 次锁定 30 秒）', () => {
  it('RATE_LIMITED 触发倒计时并阻止再次提交', async () => {
    vi.useFakeTimers()
    harness = installFetch([failResponse({ status: 429, code: 19, userMessage: '请求过于频繁' })])

    await expect(auth.login({ account: 'admin', password: 'wrong' })).resolves.toBe(false)
    expect(auth.isLocked).toBe(true)
    expect(auth.lockedSeconds).toBe(LOGIN_LOCK_SECONDS)
    expect(auth.error).toContain('锁定')

    // 锁定期间不再发起请求
    await expect(auth.login({ account: 'admin', password: 'wrong-again' })).resolves.toBe(false)
    expect(harness.count()).toBe(1)

    vi.advanceTimersByTime(LOGIN_LOCK_SECONDS * 1000)
    expect(auth.isLocked).toBe(false)
    expect(auth.lockedSeconds).toBe(0)
    vi.useRealTimers()
  })

  it('账号或密码错误（UNAUTHORIZED）只提示失败原因，不锁定', async () => {
    harness = installFetch([failResponse({ status: 401, code: 401, userMessage: '账号或密码错误' })])
    await expect(auth.login({ account: 'admin', password: 'wrong' })).resolves.toBe(false)
    expect(auth.isLocked).toBe(false)
    expect(auth.error).toBe('账号或密码错误')
  })
})

describe('重新验证（REAUTH_REQUIRED，Go 契约休眠路径）', () => {
  it('Go 契约无重新验证端点：密码提交后显式失败，不产生第二次业务写入', async () => {
    auth.setReauthProvider(async () => 'admin-password')
    const users = useUsersStore()
    users.items = [{ id: 7, phoneMasked: '138****8888', status: 1, statusText: '正常', version: 3 }]

    // 模拟未来契约返回 REAUTH_REQUIRED(23)：弹窗提交密码后，因 Go 暂无 reauth 端点而显式失败。
    harness = installFetch([failResponse({ status: 401, code: 23, userMessage: '需要重新验证管理员密码' })])

    await expect(users.setStatus(users.items[0], 0, '人工审核冻结')).resolves.toBe(false)
    expect(harness.count()).toBe(1)
    expect(users.error).toContain('暂未提供')
  })

  it('弹窗提交空密码时保留弹窗并提示', async () => {
    const pending = auth.ensureReauth()
    expect(auth.reauthActive).toBe(true)
    expect(auth.submitReauth('')).toBe(false)
    expect(auth.reauthActive).toBe(true)
    expect(auth.reauthError).toBe('请输入当前登录密码')

    expect(auth.submitReauth('secret')).toBe(true)
    await expect(pending).resolves.toBe('secret')
    expect(auth.reauthActive).toBe(false)
  })

  it('取消重新验证时以 null 释放等待者', async () => {
    const pending = auth.ensureReauth()
    auth.cancelReauth()
    await expect(pending).resolves.toBeNull()
  })
})

describe('会话失效处理', () => {
  it('handleSessionExpired 清理令牌并置位 sessionExpired', async () => {
    harness = installFetch([loginEnvelope()])
    await auth.login({ account: 'admin', password: 'example-password' })

    auth.handleSessionExpired('登录已失效，请重新登录')
    expect(auth.token).toBeNull()
    expect(auth.sessionExpired).toBe(true)
    expect(auth.error).toBe('登录已失效，请重新登录')
    expect(sessionStorage.getItem(TOKEN_STORAGE_KEY)).toBeNull()
  })
})
