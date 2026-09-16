import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import OpsView from '../src/views/OpsView.vue'
import { useAuthStore } from '../src/stores/auth'
import { useOpsStore } from '../src/stores/ops'
import { failResponse, installFetch, okResponse, settle } from './helpers'

/** Go AuditEntry 契约字段（resource 命名 + ISO 时间）。 */
const GO_AUDIT_ITEMS = [
  {
    id: 1,
    actorType: 'ADMIN',
    actorId: '1',
    action: 'USER_FROZEN',
    resourceType: 'USER',
    resourceId: '7',
    requestId: 'req-a',
    payload: '{"reason":"人工审核冻结"}',
    createdAt: '2026-09-02T12:00:00Z'
  },
  {
    id: 2,
    actorType: 'ADMIN',
    actorId: '2',
    action: 'STATION_CREATED',
    resourceType: 'STATION',
    resourceId: '5',
    requestId: 'req-b',
    payload: '{}',
    createdAt: '2026-09-02T13:00:00Z'
  }
]

/** 以管理员身份挂载运维页（Go 契约：审计对所有管理员可读）。 */
function mountOps({ adminRole = 'SUPER_ADMIN', username = 'admin' } = {}) {
  const pinia = createPinia()
  setActivePinia(pinia)
  const auth = useAuthStore()
  auth.token = 'admin-token'
  auth.admin = { id: 1, username, roles: [adminRole], status: 'ACTIVE', mustChangePassword: false }
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

describe('审计日志（Go 契约）', () => {
  it('渲染审计行，初次挂载只请求审计一个接口', async () => {
    harness = installFetch([okResponse({ items: GO_AUDIT_ITEMS, meta: { page: 1, pageSize: 20, total: 2 } })])
    const { wrapper } = mountOps()
    await settle()

    expect(harness.urlOf(0).startsWith('/api/v1/admin/audit')).toBe(true)
    expect(harness.methodOf(0)).toBe('GET')
    expect(harness.count()).toBe(1)

    // 审计行 data-testid：audit-row-<id>-<action>
    expect(wrapper.find('[data-testid="audit-row-1-USER_FROZEN"]').exists()).toBe(true)
    expect(wrapper.get('[data-testid="audit-row-2-STATION_CREATED"]').text()).toContain('STATION_CREATED')
    expect(wrapper.get('[data-testid="audit-row-1-USER_FROZEN"]').text()).toContain('USER')
  })

  it('空数据显示空状态，失败显示错误并可重试', async () => {
    harness = installFetch([okResponse({ items: [], meta: { page: 1, pageSize: 20, total: 0 } })])
    const first = mountOps()
    await settle()
    expect(first.wrapper.find('[data-testid="ops-empty"]').exists()).toBe(true)

    harness.always(failResponse({ status: 500, code: 13, userMessage: '未预期内部错误' }))
    const second = mountOps()
    await settle()
    expect(second.wrapper.get('[data-testid="ops-error"]').text()).toContain('未预期内部错误')

    await second.wrapper.get('[data-testid="ops-error"] button').trigger('click')
    await settle()
    expect(harness.count()).toBeGreaterThan(2)
  })

  it('审计过滤按 Go 契约参数提交（resource 命名，无时间范围）', async () => {
    harness = installFetch([okResponse({ items: [], meta: { page: 1, pageSize: 20, total: 0 } })])
    const { wrapper, ops } = mountOps()
    await settle()

    await wrapper.get('[data-testid="ops-actor"]').setValue('1')
    await wrapper.get('[data-testid="ops-action"]').setValue('USER_FROZEN')
    await wrapper.get('[data-testid="ops-target-type"]').setValue('USER')
    await wrapper.get('[data-testid="ops-target-id"]').setValue('7')
    await wrapper.get('[data-testid="ops-filter"]').trigger('submit')
    await settle()

    const query = harness.queryOf(harness.count() - 1)
    expect(query.get('actorId')).toBe('1')
    expect(query.get('action')).toBe('USER_FROZEN')
    expect(query.get('resourceType')).toBe('USER')
    expect(query.get('resourceId')).toBe('7')
    expect(query.has('fromAt')).toBe(false)
    expect(query.has('toAt')).toBe(false)
    expect(ops.auditFilters.action).toBe('USER_FROZEN')
  })
})

describe('一致性备份（Go 契约暂缺）', () => {
  it('备份区显式提示未开放，不发起任何请求', async () => {
    harness = installFetch([okResponse({ items: GO_AUDIT_ITEMS, meta: { page: 1, pageSize: 20, total: 2 } })])
    const { wrapper } = mountOps()
    await settle()

    expect(wrapper.find('[data-testid="ops-backups-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="backup-create"]').exists()).toBe(false)
    expect(harness.count()).toBe(1)
  })
})
