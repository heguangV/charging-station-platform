import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useUsersStore } from '../src/stores/users'
import { failResponse, installFetch, okResponse } from './helpers'

const USER_ROW = {
  id: 7,
  username: 'driver_lee',
  phoneMasked: '138****8888',
  nickname: '李先生',
  status: 1,
  balanceCent: 12800,
  debtCent: 0,
  registeredAt: 1788134400
}

const USER_DETAIL = { ...USER_ROW, statusText: '正常', version: 3, activeSessionCount: 2, hasActiveFlow: true }

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

describe('用户查询', () => {
  it('完整手机号必须是 11 位数字，非法输入不发请求', async () => {
    harness = installFetch([okResponse({ items: [], total: 0, page: 1, pageSize: 20 })])
    await users.setFilter({ phoneExact: '1380013' })
    expect(users.error).toBe('完整手机号必须是 11 位数字')
    expect(harness.count()).toBe(0)
  })

  it('后四位与完整手机号不能同时提交', async () => {
    harness = installFetch([okResponse({ items: [], total: 0, page: 1, pageSize: 20 })])
    await users.setFilter({ phoneExact: '13800138000', phoneLast4: '8000' })
    expect(users.error).toBe('完整手机号与后四位只能选择一种查询方式')
    expect(harness.count()).toBe(0)
  })

  it('合法的后四位进入查询参数，排序使用白名单值', async () => {
    harness = installFetch([okResponse({ items: [USER_ROW], total: 1, page: 1, pageSize: 20 })])
    await users.setFilter({ phoneExact: '', phoneLast4: '8888', status: 1, sort: '-balanceCent' })

    const query = harness.queryOf(0)
    expect(harness.urlOf(0).startsWith('/api/v1/admin/users')).toBe(true)
    expect(query.get('phoneLast4')).toBe('8888')
    expect(query.has('phoneExact')).toBe(false)
    expect(query.get('status')).toBe('1')
    expect(query.get('sort')).toBe('-balanceCent')
    expect(users.total).toBe(1)
  })

  it('空结果与加载失败分别落到空状态与错误文案', async () => {
    harness = installFetch([okResponse({ items: [], total: 0, page: 1, pageSize: 20 })])
    await users.load()
    expect(users.isEmpty).toBe(true)
    expect(users.error).toBe('')

    harness.always(failResponse({ status: 500, code: 13, userMessage: '未预期内部错误' }))
    await users.load()
    expect(users.error).toBe('未预期内部错误')
    expect(users.items).toEqual([])
  })
})

describe('用户详情与订单历史', () => {
  it('用户详情包含脱敏手机号、会话数与活动流程摘要', async () => {
    harness = installFetch([okResponse(USER_DETAIL)])
    await users.loadDetail(7)
    expect(harness.urlOf(0)).toBe('/api/v1/admin/users/7')
    expect(users.detail.phoneMasked).toBe('138****8888')
    expect(users.detail.activeSessionCount).toBe(2)
    expect(users.detail.hasActiveFlow).toBe(true)
    expect(users.detail.version).toBe(3)
  })

  it('订单历史按用户维度请求，并支持状态过滤', async () => {
    harness = installFetch([
      okResponse({
        items: [{ orderNo: 'ORD-1', stationName: '中关村站', chargerCode: 'ZGC-DC-01', status: 60, statusText: '已完成', energyMwh: 12500000, amountCent: 3200 }],
        total: 1,
        page: 1,
        pageSize: 20
      })
    ])
    await users.loadOrders(7, { status: 60 })
    expect(harness.urlOf(0).startsWith('/api/v1/admin/users/7/orders')).toBe(true)
    expect(harness.queryOf(0).get('status')).toBe('60')
    expect(users.orders[0].orderNo).toBe('ORD-1')
    expect(users.ordersError).toBe('')
  })

  it('订单历史失败时清空列表并给出错误', async () => {
    harness = installFetch([failResponse({ status: 403, code: 403, userMessage: '无权访问该资源' })])
    await users.loadOrders(7)
    expect(users.orders).toEqual([])
    expect(users.ordersError).toBe('无权访问该资源')
  })
})

describe('冻结与解冻', () => {
  it('使用详情返回的 version 提交，成功后就地更新列表与详情', async () => {
    harness = installFetch([
      okResponse(USER_DETAIL),
      okResponse({ id: 7, status: 0, statusText: '冻结', version: 4, activeFlowPreserved: true })
    ])
    users.items = [{ ...USER_ROW }]
    await users.loadDetail(7)
    await expect(users.setStatus(users.detail, 0, '人工审核冻结')).resolves.toBe(true)

    const index = harness.indexOf('PUT', '/admin/users/7/status')
    expect(harness.bodyOf(index)).toEqual({ status: 0, reason: '人工审核冻结', version: 3 })
    expect(harness.headersOf(index)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(users.items[0].status).toBe(0)
    expect(users.items[0].statusText).toBe('冻结')
    expect(users.items[0].version).toBe(4)
    expect(users.detail.status).toBe(0)
    // 冻结不取消进行中的充电，服务端通过 activeFlowPreserved 明确告知
    expect(users.activeFlowPreserved).toBe(true)
    expect(users.notice).toContain('已冻结')
  })

  it('解冻写回正常状态', async () => {
    harness = installFetch([okResponse({ id: 7, status: 1, statusText: '正常', version: 5, activeFlowPreserved: false })])
    users.items = [{ ...USER_ROW, status: 0, statusText: '冻结', version: 4 }]
    await expect(users.setStatus(users.items[0], 1, '申诉通过解冻')).resolves.toBe(true)
    expect(users.items[0].status).toBe(1)
    expect(users.notice).toContain('已解冻')
  })

  it('版本冲突时提示并重载用户列表', async () => {
    harness = installFetch([
      failResponse({ status: 409, code: 22, userMessage: '版本落后' }),
      okResponse({ items: [{ ...USER_ROW, version: 9 }], total: 1, page: 1, pageSize: 20 })
    ])
    users.items = [{ ...USER_ROW, version: 1 }]
    await expect(users.setStatus(users.items[0], 0, '人工审核冻结')).resolves.toBe(false)
    expect(users.conflict).toContain('其他管理员')
    expect(harness.indexOf('GET', '/admin/users')).toBe(1)
    expect(users.items[0].version).toBe(9)
  })
})
