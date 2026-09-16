import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useUsersStore } from '../src/stores/users'
import { failResponse, installFetch, okResponse } from './helpers'

/** Go UserSummary 契约字段（列表不含手机号，隐私基线：无模糊扫描）。 */
const GO_USER = {
  id: 7,
  displayName: '李先生',
  status: 'ACTIVE',
  balanceCent: 12800
}

/** Go UserDetail 契约字段。 */
const GO_USER_DETAIL = {
  id: 7,
  phone: '138****8888',
  displayName: '李先生',
  avatarUrl: '',
  status: 'ACTIVE',
  balanceCent: 12800,
  registeredAt: '2026-09-02T12:00:00Z'
}

/** 适配层归一化后的行形状。 */
const USER_ROW = {
  id: 7,
  displayName: '李先生',
  status: 1,
  statusText: '正常',
  balanceCent: 12800,
  version: 0
}

function goPage(items, total = items.length) {
  return { items, meta: { page: 1, pageSize: 20, total } }
}

let users = null
let harness = null

beforeEach(() => {
  setActivePinia(createPinia())
  sessionStorage.clear()
  localStorage.clear()
  users = useUsersStore()
})

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('用户查询（Go 契约）', () => {
  it('keyword/status 过滤；status 数字码映射为 Go 枚举', async () => {
    harness = installFetch([okResponse(goPage([GO_USER]))])
    await users.setFilter({ keyword: '李', status: 1 })

    const query = harness.queryOf(0)
    expect(harness.urlOf(0).startsWith('/api/v1/admin/users')).toBe(true)
    expect(query.get('keyword')).toBe('李')
    expect(query.get('status')).toBe('ACTIVE')
    expect(users.total).toBe(1)
    expect(users.items[0].status).toBe(1)
    expect(users.items[0].statusText).toBe('正常')
  })

  it('空结果与加载失败分别落到空状态与错误文案', async () => {
    harness = installFetch([okResponse(goPage([]))])
    await users.load()
    expect(users.isEmpty).toBe(true)
    expect(users.error).toBe('')

    harness.always(failResponse({ status: 500, code: 13, userMessage: '未预期内部错误' }))
    await users.load()
    expect(users.error).toBe('未预期内部错误')
    expect(users.items).toEqual([])
  })
})

describe('用户详情与账务', () => {
  it('用户详情包含脱敏手机号、余额与注册时间（ISO → 秒）', async () => {
    harness = installFetch([okResponse(GO_USER_DETAIL)])
    await users.loadDetail(7)
    expect(harness.urlOf(0)).toBe('/api/v1/admin/users/7')
    expect(users.detail.phone).toBe('138****8888')
    expect(users.detail.status).toBe(1)
    expect(users.detail.registeredAt).toBe(1788350400)
    // Go 契约没有会话数/活动流程摘要/版本乐观锁。
    expect(users.detail.version).toBe(0)
  })

  it('订单历史按用户过滤请求 /admin/orders?userId=，状态映射为 Go 枚举', async () => {
    harness = installFetch([
      okResponse(goPage([
        {
          orderNo: 'ORD-1', userId: 7, stationId: 1, chargerId: 11, status: 'COMPLETED',
          amountCent: 3200, energyWh: 12500, createdAt: '2026-09-02T12:00:00Z', updatedAt: '2026-09-02T13:00:00Z'
        }
      ]))
    ])
    await users.loadOrders(7, { status: 60 })
    expect(harness.urlOf(0).startsWith('/api/v1/admin/orders')).toBe(true)
    expect(harness.queryOf(0).get('userId')).toBe('7')
    expect(harness.queryOf(0).get('status')).toBe('COMPLETED')
    expect(users.orders[0].orderNo).toBe('ORD-1')
    expect(users.orders[0].statusText).toBe('已完成')
    expect(users.orders[0].energyMwh).toBe(12500000)
    expect(users.ordersError).toBe('')
  })

  it('用户账务查询走 /admin/users/{id}/transactions', async () => {
    harness = installFetch([
      okResponse(goPage([
        { id: 1, transactionType: 'TOP_UP', amountCent: 10000, balanceBeforeCent: 0, balanceAfterCent: 10000, createdAt: '2026-09-02T12:00:00Z' }
      ]))
    ])
    await users.loadTransactions(7, { type: 'TOP_UP' })
    expect(harness.urlOf(0)).toBe('/api/v1/admin/users/7/transactions?type=TOP_UP&page=1&pageSize=20')
    expect(users.transactions[0].transactionType).toBe('TOP_UP')
  })
})

describe('冻结与解冻（Go 契约：两个 POST 端点，无 body）', () => {
  it('冻结走 /freeze，成功后就地更新列表与详情', async () => {
    harness = installFetch([
      okResponse(GO_USER_DETAIL),
      okResponse({ id: 7, status: 'DISABLED' })
    ])
    users.items = [{ ...USER_ROW }]
    await users.loadDetail(7)
    await expect(users.setStatus(users.detail, 0, '人工审核冻结')).resolves.toBe(true)

    const index = harness.indexOf('POST', '/admin/users/7/freeze')
    expect(index).toBe(1)
    expect(harness.headersOf(index)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(users.items[0].status).toBe(0)
    expect(users.items[0].statusText).toBe('冻结')
    expect(users.detail.status).toBe(0)
    expect(users.notice).toContain('已冻结')
  })

  it('解冻走 /unfreeze 并写回正常状态', async () => {
    harness = installFetch([okResponse({ id: 7, status: 'ACTIVE' })])
    users.items = [{ ...USER_ROW, status: 0, statusText: '冻结' }]
    await expect(users.setStatus(users.items[0], 1, '申诉通过解冻')).resolves.toBe(true)
    expect(harness.indexOf('POST', '/admin/users/7/unfreeze')).toBe(0)
    expect(users.items[0].status).toBe(1)
    expect(users.notice).toContain('已解冻')
  })

  it('服务端失败时保留可读错误', async () => {
    harness = installFetch([failResponse({ status: 404, code: 4, userMessage: '用户不存在' })])
    users.items = [{ ...USER_ROW, status: 1 }]
    await expect(users.setStatus(users.items[0], 0, '人工审核冻结')).resolves.toBe(false)
    expect(users.error).toBe('用户不存在')
  })
})

describe('手工建档（Go 契约）', () => {
  /** Go UserRecord 契约字段（POST /admin/users 的 201 响应）。 */
  const GO_USER_RECORD = {
    id: 31,
    phone: '13912340000',
    displayName: '老王',
    status: 'ACTIVE',
    balanceCent: 0
  }

  it('建档走 POST /admin/users；空昵称不下发，成功后重载列表', async () => {
    harness = installFetch([okResponse(GO_USER_RECORD), okResponse(goPage([]))])

    await expect(users.createUser({ phone: '13912340000', displayName: '' })).resolves.toBe(true)

    const index = harness.indexOf('POST', '/admin/users')
    expect(index).toBe(0)
    // 空昵称不发送：昵称由服务端取注册默认（用户+手机号后四位）。
    expect(harness.bodyOf(index)).toEqual({ phone: '13912340000' })
    expect(harness.headersOf(index)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(users.notice).toContain('#31')
  })

  it('手机号已注册（409 ALREADY_EXISTS）按业务冲突提示', async () => {
    harness = installFetch([failResponse({ status: 409, code: 5, userMessage: '该手机号已注册' })])

    await expect(users.createUser({ phone: '13912340000', displayName: '老王' })).resolves.toBe(false)
    expect(users.conflict).toContain('已注册')
  })

  it('批量建档走 POST /admin/users/batch，整页提交并按 userCount 提示', async () => {
    harness = installFetch([
      okResponse({
        userCount: 2,
        created: [
          { id: 32, phone: '13912340001', displayName: '用户0001', status: 'ACTIVE', balanceCent: 0 },
          { id: 33, phone: '13912340002', displayName: '小李', status: 'ACTIVE', balanceCent: 0 }
        ]
      }),
      okResponse(goPage([]))
    ])

    await expect(
      users.batchCreateUsers([
        { phone: '13912340001', displayName: '' },
        { phone: '13912340002', displayName: '小李' }
      ])
    ).resolves.toBe(true)

    const index = harness.indexOf('POST', '/admin/users/batch')
    expect(index).toBe(0)
    expect(harness.bodyOf(index)).toEqual({
      users: [
        { phone: '13912340001', displayName: undefined },
        { phone: '13912340002', displayName: '小李' }
      ]
    })
    expect(harness.headersOf(index)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(users.notice).toContain('2')
  })

  it('批量页内存在已注册手机号（409）时整页拒绝', async () => {
    harness = installFetch([failResponse({ status: 409, code: 5, userMessage: '页内存在已注册的手机号' })])

    await expect(
      users.batchCreateUsers([{ phone: '13912340000', displayName: '' }])
    ).resolves.toBe(false)
    expect(users.conflict).toContain('整页未创建')
  })
})
