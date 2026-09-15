import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import OpsView from '../src/views/OpsView.vue'
import { useAuthStore } from '../src/stores/auth'
import { useOpsStore } from '../src/stores/ops'
import { failResponse, installFetch, okResponse, settle } from './helpers'

const AUDIT_ITEMS = [
  {
    actorId: 'admin:1',
    action: 'USER_FROZEN',
    targetType: 'USER',
    targetId: '7',
    reason: '人工审核冻结',
    at: 1788235200
  },
  {
    actorId: 'admin:2',
    action: 'ADMIN_CREATED',
    targetType: 'ADMIN',
    targetId: '5',
    reason: '新入职运营专员',
    at: 1788238800
  }
]

const BACKUPS = [
  {
    backupNo: 'BK20260902001',
    status: 'READY',
    checksum: 'a1b2c3d4e5f60718293a4b5c6d7e8f90',
    sizeBytes: 204800,
    createdAt: 1788235200,
    verificationStatus: 'SUCCEEDED'
  }
]

/** 以指定角色挂载运维页（审计与备份都需要 OWNER 权限）。 */
function mountOps({ roles = ['OWNER'], username = 'admin' } = {}) {
  const pinia = createPinia()
  setActivePinia(pinia)
  const auth = useAuthStore()
  auth.token = 'admin-token'
  auth.admin = { id: 1, username, roles, status: 1, mustChangePassword: false, version: 1 }
  const wrapper = mount(OpsView, { global: { plugins: [pinia] } })
  return { wrapper, auth, ops: useOpsStore() }
}

let harness = null

beforeEach(() => {
  sessionStorage.clear()
  localStorage.clear()
})

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('审计日志与备份列表', () => {
  it('渲染审计行与备份行，初次挂载只请求两个只读接口', async () => {
    harness = installFetch([okResponse({ items: AUDIT_ITEMS, page: 1, pageSize: 20 }), okResponse({ items: BACKUPS })])
    const { wrapper } = mountOps()
    await settle()

    expect(harness.urlOf(0).startsWith('/api/v1/admin/audit-logs')).toBe(true)
    expect(harness.urlOf(1)).toBe('/api/v1/admin/backups')
    expect(harness.methodOf(0)).toBe('GET')
    expect(harness.count()).toBe(2)

    // 审计行 data-testid：audit-row-<at>-<action>
    expect(wrapper.find('[data-testid="audit-row-1788235200-USER_FROZEN"]').exists()).toBe(true)
    expect(wrapper.get('[data-testid="audit-row-1788238800-ADMIN_CREATED"]').text()).toContain('ADMIN_CREATED')
    expect(wrapper.get('[data-testid="audit-row-1788235200-USER_FROZEN"]').text()).toContain('人工审核冻结')
    expect(wrapper.get('[data-testid="backup-row-BK20260902001"]').text()).toContain('200.0 KB')
    expect(wrapper.get('[data-testid="ops-audit-range"]').text()).toContain('第 1-2 条')
  })

  it('空数据显示空状态，失败显示错误并可重试', async () => {
    harness = installFetch([okResponse({ items: [], page: 1, pageSize: 20 }), okResponse({ items: [] })])
    const first = mountOps()
    await settle()
    expect(first.wrapper.find('[data-testid="ops-empty"]').exists()).toBe(true)
    expect(first.wrapper.find('[data-testid="ops-backups-empty"]').exists()).toBe(true)

    harness.always(failResponse({ status: 403, code: 403, userMessage: '需要 OWNER 权限' }))
    const second = mountOps()
    await settle()
    expect(second.wrapper.get('[data-testid="ops-error"]').text()).toContain('需要 OWNER 权限')

    await second.wrapper.get('[data-testid="ops-error"] button').trigger('click')
    await settle()
    expect(harness.count()).toBeGreaterThan(4)
  })

  it('非 OWNER 账号给出权限提示且不请求接口', async () => {
    harness = installFetch([okResponse({ items: [], page: 1, pageSize: 20 })])
    const { wrapper, auth } = mountOps({ roles: ['OPERATOR'], username: 'ops_wang' })
    await settle()

    expect(auth.isOwner).toBe(false)
    expect(wrapper.find('[data-testid="ops-permission-hint"]').exists()).toBe(true)
    expect(harness.count()).toBe(0)
  })

  it('审计过滤条件按参数提交', async () => {
    harness = installFetch([okResponse({ items: [], page: 1, pageSize: 20 }), okResponse({ items: [] })])
    const { wrapper, ops } = mountOps()
    await settle()

    await wrapper.get('[data-testid="ops-actor"]').setValue('admin:1')
    await wrapper.get('[data-testid="ops-action"]').setValue('USER_FROZEN')
    await wrapper.get('[data-testid="ops-filter"]').trigger('submit')
    await settle()

    const query = harness.queryOf(harness.count() - 1)
    expect(query.get('actorId')).toBe('admin:1')
    expect(query.get('action')).toBe('USER_FROZEN')
    expect(ops.auditFilters.action).toBe('USER_FROZEN')
  })
})

describe('创建备份', () => {
  it('必须先二次确认，确认后才 POST /admin/backups 并携带幂等键', async () => {
    harness = installFetch([
      okResponse({ items: AUDIT_ITEMS, page: 1, pageSize: 20 }),
      okResponse({ items: BACKUPS }),
      okResponse({ backupNo: 'BK20260902002', status: 'READY', checksum: 'ffeeddccbbaa99887766554433221100' }),
      okResponse({ items: [{ ...BACKUPS[0], backupNo: 'BK20260902002' }, ...BACKUPS] })
    ])
    const { wrapper } = mountOps()
    await settle()
    expect(harness.indexOf('POST', '/admin/backups')).toBe(-1)

    await wrapper.get('[data-testid="backup-create"]').trigger('click')
    const dialog = wrapper.get('[data-testid="confirm-dialog"]')
    expect(dialog.text()).toContain('创建一致性备份')
    // 仅打开确认框不会产生写入
    expect(harness.indexOf('POST', '/admin/backups')).toBe(-1)

    await wrapper.get('[data-testid="confirm-submit"]').trigger('click')
    await settle()

    const index = harness.indexOf('POST', '/admin/backups')
    expect(index).toBeGreaterThan(0)
    expect(harness.headersOf(index)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    // 创建成功后重新拉取备份记录
    expect(
      harness.calls.filter(call => call.options.method === 'GET' && String(call.url) === '/api/v1/admin/backups')
    ).toHaveLength(2)
    expect(wrapper.find('[data-testid="confirm-dialog"]').exists()).toBe(false)
    expect(wrapper.text()).toContain('BK20260902002')
  })

  it('取消确认时不发起创建请求', async () => {
    harness = installFetch([okResponse({ items: [], page: 1, pageSize: 20 }), okResponse({ items: [] })])
    const { wrapper } = mountOps()
    await settle()

    await wrapper.get('[data-testid="backup-create"]').trigger('click')
    await wrapper.get('[data-testid="confirm-cancel"]').trigger('click')
    await settle()

    expect(wrapper.find('[data-testid="confirm-dialog"]').exists()).toBe(false)
    expect(harness.indexOf('POST', '/admin/backups')).toBe(-1)
  })

  it('服务端要求重新验证时先弹窗等待密码，验证后用同一幂等键重试一次', async () => {
    harness = installFetch([
      okResponse({ items: [], page: 1, pageSize: 20 }),
      okResponse({ items: BACKUPS }),
      failResponse({ status: 401, code: 23, userMessage: '需要重新验证管理员密码' }),
      okResponse({ reauthExpiresAt: 1893456000 }),
      okResponse({ backupNo: 'BK20260902003', status: 'READY', checksum: '00112233445566778899aabbccddeeff' }),
      okResponse({ items: [{ ...BACKUPS[0], backupNo: 'BK20260902003' }] })
    ])
    const { wrapper, auth, ops } = mountOps()
    await settle()

    await wrapper.get('[data-testid="backup-create"]').trigger('click')
    await wrapper.get('[data-testid="confirm-submit"]').trigger('click')
    await settle()

    // 等待管理员输入密码：确认框保持打开，重新验证进入激活状态
    expect(auth.reauthActive).toBe(true)
    expect(wrapper.find('[data-testid="confirm-dialog"]').exists()).toBe(true)

    auth.submitReauth('admin-password')
    await settle()

    const posts = harness.calls.filter(
      call => call.options.method === 'POST' && String(call.url) === '/api/v1/admin/backups'
    )
    expect(posts).toHaveLength(2)
    expect(posts[1].options.headers['Idempotency-Key']).toBe(posts[0].options.headers['Idempotency-Key'])
    expect(harness.indexOf('POST', '/admin/auth/reauth')).toBeGreaterThan(0)
    expect(auth.reauthExpiresAt).toBe(1893456000)
    expect(ops.error).toBe('')
    expect(wrapper.find('[data-testid="confirm-dialog"]').exists()).toBe(false)
    expect(ops.notice).toContain('BK20260902003')
  })

  it('创建失败（非重新验证）时保留弹窗并回显服务端提示', async () => {
    harness = installFetch([
      okResponse({ items: [], page: 1, pageSize: 20 }),
      okResponse({ items: BACKUPS }),
      failResponse({ status: 503, code: 11, userMessage: '事务安全回滚' })
    ])
    const { wrapper, ops } = mountOps()
    await settle()

    await wrapper.get('[data-testid="backup-create"]').trigger('click')
    await wrapper.get('[data-testid="confirm-submit"]').trigger('click')
    await settle()

    expect(ops.error).toBe('事务安全回滚')
    expect(wrapper.get('[data-testid="confirm-error"]').text()).toContain('事务安全回滚')
    expect(wrapper.find('[data-testid="confirm-dialog"]').exists()).toBe(true)
  })
})

describe('隔离恢复验证', () => {
  it('验证动作 POST 到 /admin/backups/{backupNo}/verifications 并刷新备份列表', async () => {
    harness = installFetch([
      okResponse({ items: AUDIT_ITEMS, page: 1, pageSize: 20 }),
      okResponse({ items: BACKUPS }),
      okResponse({ backupNo: 'BK20260902001', verificationStatus: 'SUCCEEDED' }),
      okResponse({ items: [{ ...BACKUPS[0], verificationStatus: 'SUCCEEDED' }] })
    ])
    const { wrapper, ops } = mountOps()
    await settle()

    await wrapper.get('[data-testid="backup-verify-BK20260902001"]').trigger('click')
    await settle()

    const index = harness.indexOf('POST', '/admin/backups/BK20260902001/verifications')
    expect(index).toBeGreaterThan(0)
    expect(harness.headersOf(index)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(ops.verification).toEqual({ backupNo: 'BK20260902001', verificationStatus: 'SUCCEEDED' })
    expect(wrapper.get('[data-testid="backup-verification"]').text()).toContain('SUCCEEDED')
    expect(
      harness.calls.filter(call => call.options.method === 'GET' && String(call.url) === '/api/v1/admin/backups')
    ).toHaveLength(2)
  })

  it('验证失败时给出错误提示', async () => {
    harness = installFetch([
      okResponse({ items: [], page: 1, pageSize: 20 }),
      okResponse({ items: BACKUPS }),
      failResponse({ status: 503, code: 11, userMessage: '事务安全回滚' })
    ])
    const { wrapper, ops } = mountOps()
    await settle()

    await wrapper.get('[data-testid="backup-verify-BK20260902001"]').trigger('click')
    await settle()

    expect(ops.error).toBe('事务安全回滚')
    expect(wrapper.get('[data-testid="ops-error-banner"]').text()).toContain('事务安全回滚')
  })
})
