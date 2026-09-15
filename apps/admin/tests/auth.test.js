import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useAuthStore, LOGIN_LOCK_SECONDS } from '../src/stores/auth'
import { useUsersStore } from '../src/stores/users'
import { TOKEN_STORAGE_KEY, clearAccessToken, getAccessToken } from '../src/api/http'
import { failResponse, flush, installFetch, okResponse } from './helpers'

/** 每个用例自行安装的 fetch 替身（用于断言请求次数与请求头）。 */
let harness = null

const ADMIN = {
  id: 1,
  username: 'admin',
  roles: ['OWNER'],
  status: 1,
  mustChangePassword: false,
  version: 1
}

function loginEnvelope(admin = ADMIN) {
  return okResponse({ accessToken: 'admin-token-1', expiresAt: 1893456000, sessionId: 11, admin })
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
    await expect(auth.login({ username: 'admin', password: 'example-password' })).resolves.toBe(true)

    expect(auth.token).toBe('admin-token-1')
    expect(getAccessToken()).toBe('admin-token-1')
    expect(sessionStorage.getItem(TOKEN_STORAGE_KEY)).toBe('admin-token-1')
    expect(localStorage.length).toBe(0)
    expect(localStorage.getItem(TOKEN_STORAGE_KEY)).toBeNull()
    expect(auth.isLoggedIn).toBe(true)
    expect(auth.displayName).toBe('admin')
    expect(auth.isOwner).toBe(true)
  })

  it('登录请求体包含账号、密码与设备标识', async () => {
    harness = installFetch([loginEnvelope()])
    await auth.login({ username: 'ops_wang', password: 'ncs-Initial-2026' })
    expect(harness.bodyOf(0)).toEqual({
      username: 'ops_wang',
      password: 'ncs-Initial-2026',
      deviceId: 'ncs-admin-web'
    })
    expect(harness.urlOf(0)).toBe('/api/v1/admin/auth/login')
  })

  it('刷新页面后可从 sessionStorage 恢复令牌与资料快照', async () => {
    harness = installFetch([loginEnvelope()])
    await auth.login({ username: 'admin', password: 'example-password' })

    const restored = useAuthStore()
    restored.$reset()
    restored.token = null
    restored.admin = null

    expect(restored.restore()).toBe(true)
    expect(restored.isLoggedIn).toBe(true)
    expect(restored.displayName).toBe('admin')
    expect(restored.roles).toEqual(['OWNER'])
  })
})

describe('退出与首次改密', () => {
  it('退出清理令牌、资料与 sessionStorage', async () => {
    harness = installFetch([loginEnvelope(), okResponse({})])
    await auth.login({ username: 'admin', password: 'example-password' })
    await auth.logout()

    expect(auth.token).toBeNull()
    expect(auth.admin).toBeNull()
    expect(auth.notice).toBe('已退出登录')
    expect(sessionStorage.getItem(TOKEN_STORAGE_KEY)).toBeNull()
    expect(harness.urlOf(1)).toBe('/api/v1/admin/auth/logout')
  })

  it('退出接口失败也要清理本地会话', async () => {
    harness = installFetch([loginEnvelope(), failResponse({ status: 500, code: 13, userMessage: '服务异常' })])
    await auth.login({ username: 'admin', password: 'example-password' })
    await auth.logout()
    expect(auth.token).toBeNull()
    expect(getAccessToken()).toBeNull()
  })

  it('mustChangePassword=true 时暴露改密状态，改密成功后清除', async () => {
    harness = installFetch([
      loginEnvelope({ ...ADMIN, mustChangePassword: true }),
      okResponse({ ...ADMIN, mustChangePassword: false, version: 2 })
    ])
    await auth.login({ username: 'ops_wang', password: 'ncs-Initial-2026' })
    expect(auth.mustChangePassword).toBe(true)
    expect(auth.notice).toContain('首次登录')

    await expect(
      auth.changeOwnPassword({ currentPassword: 'ncs-Initial-2026', newPassword: 'ncs-New-2026-pw' })
    ).resolves.toBe(true)
    expect(auth.mustChangePassword).toBe(false)
    expect(harness.urlOf(1)).toBe('/api/v1/admin/me/password')
    // 修改本人密码按文档不需要幂等键（§6.11）。
    expect(harness.headersOf(1)['Idempotency-Key']).toBeUndefined()
  })
})

describe('登录锁定（连续失败 5 次锁定 30 秒）', () => {
  it('RATE_LIMITED 触发倒计时并阻止再次提交', async () => {
    vi.useFakeTimers()
    harness = installFetch([failResponse({ status: 429, code: 19, userMessage: '请求过于频繁' })])

    await expect(auth.login({ username: 'admin', password: 'wrong' })).resolves.toBe(false)
    expect(auth.isLocked).toBe(true)
    expect(auth.lockedSeconds).toBe(LOGIN_LOCK_SECONDS)
    expect(auth.error).toContain('锁定')

    // 锁定期间不再发起请求
    await expect(auth.login({ username: 'admin', password: 'wrong-again' })).resolves.toBe(false)
    expect(harness.count()).toBe(1)

    vi.advanceTimersByTime(LOGIN_LOCK_SECONDS * 1000)
    expect(auth.isLocked).toBe(false)
    expect(auth.lockedSeconds).toBe(0)
    vi.useRealTimers()
  })

  it('账号或密码错误（UNAUTHORIZED）只提示失败原因，不锁定', async () => {
    harness = installFetch([failResponse({ status: 401, code: 401, userMessage: '账号或密码错误' })])
    await expect(auth.login({ username: 'admin', password: 'wrong' })).resolves.toBe(false)
    expect(auth.isLocked).toBe(false)
    expect(auth.error).toBe('账号或密码错误')
  })
})

describe('重新验证（REAUTH_REQUIRED）', () => {
  it('敏感写入失败后重新验证，并用同一个幂等键重试一次', async () => {
    auth.setReauthProvider(async () => 'admin-password')
    const users = useUsersStore()
    users.items = [{ id: 7, phoneMasked: '138****8888', status: 1, statusText: '正常', version: 3 }]

    harness = installFetch([
      failResponse({ status: 401, code: 23, userMessage: '需要重新验证管理员密码' }),
      okResponse({ reauthExpiresAt: 1893456000 }),
      okResponse({ id: 7, status: 0, statusText: '冻结', version: 4, activeFlowPreserved: true })
    ])

    await expect(users.setStatus(users.items[0], 0, '人工审核冻结')).resolves.toBe(true)

    const statusCall = harness.indexOf('PUT', '/admin/users/7/status')
    const reauthCall = harness.indexOf('POST', '/admin/auth/reauth')
    expect(statusCall).toBe(0)
    expect(reauthCall).toBe(1)
    expect(harness.count()).toBe(3)

    // 重试使用完全相同的幂等键，重新验证请求本身不需要幂等键。
    expect(harness.headersOf(0)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(harness.headersOf(2)['Idempotency-Key']).toBe(harness.headersOf(0)['Idempotency-Key'])
    expect(harness.headersOf(1)['Idempotency-Key']).toBeUndefined()
    expect(auth.reauthExpiresAt).toBe(1893456000)
    expect(users.items[0].status).toBe(0)
  })

  it('重试后仍要求重新验证时不再循环，只重试一次', async () => {
    auth.setReauthProvider(async () => 'admin-password')
    const users = useUsersStore()
    users.items = [{ id: 9, status: 1, statusText: '正常', version: 1 }]

    harness = installFetch([
      failResponse({ status: 401, code: 23, userMessage: '需要重新验证管理员密码' }),
      okResponse({ reauthExpiresAt: 1 }),
      failResponse({ status: 401, code: 23, userMessage: '需要重新验证管理员密码' })
    ])

    await expect(users.setStatus(users.items[0], 0, '人工审核冻结')).resolves.toBe(false)
    const statusCalls = harness.calls.filter(call => String(call.url).includes('/admin/users/9/status'))
    expect(statusCalls).toHaveLength(2)
    expect(users.error).toContain('重新验证')
  })

  it('取消重新验证时放弃原请求，不产生第二次业务写入', async () => {
    const users = useUsersStore()
    users.items = [{ id: 5, status: 1, version: 1 }]
    harness = installFetch([failResponse({ status: 401, code: 23 })])

    const pending = users.setStatus(users.items[0], 0, '人工审核冻结')
    await flush()
    expect(auth.reauthActive).toBe(true)

    auth.cancelReauth()
    await expect(pending).resolves.toBe(false)
    expect(harness.calls.filter(call => String(call.url).includes('/admin/users/5/status'))).toHaveLength(1)
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
    await auth.login({ username: 'admin', password: 'example-password' })

    auth.handleSessionExpired('登录已失效，请重新登录')
    expect(auth.token).toBeNull()
    expect(auth.sessionExpired).toBe(true)
    expect(auth.error).toBe('登录已失效，请重新登录')
    expect(sessionStorage.getItem(TOKEN_STORAGE_KEY)).toBeNull()
  })
})
