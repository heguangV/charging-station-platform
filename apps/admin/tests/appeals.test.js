import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useAppealsStore } from '../src/stores/appeals'
import { failResponse, installFetch, okResponse } from './helpers'

/** Go Appeal 契约字段。 */
const GO_APPEAL = {
  id: 1,
  orderNo: 'ORD202609020001',
  reason: '计量有误，申请复核',
  status: 'PENDING',
  orderAmountCent: 3200,
  orderPaidCent: 3200,
  createdAt: '2026-09-02T12:00:00Z'
}

function goPage(items, total = items.length) {
  return { items, meta: { page: 1, pageSize: 20, total } }
}

let appeals = null
let harness = null

beforeEach(() => {
  setActivePinia(createPinia())
  sessionStorage.clear()
  localStorage.clear()
  appeals = useAppealsStore()
})

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('申诉队列（A-04 第 8 步）', () => {
  it('加载队列并把 ISO 时间归一化为秒', async () => {
    harness = installFetch([okResponse(goPage([GO_APPEAL]))])
    await appeals.load()

    expect(harness.urlOf(0).startsWith('/api/v1/admin/appeals')).toBe(true)
    expect(appeals.items[0].createdAt).toBe(1788350400)
    expect(appeals.pendingCount).toBe(1)
    expect(appeals.error).toBe('')
  })

  it('状态过滤提交 PENDING/APPROVED 枚举，重置后不下发', async () => {
    harness = installFetch([okResponse(goPage([GO_APPEAL])), okResponse(goPage([]))])
    await appeals.setFilter({ status: 'APPROVED' })
    expect(harness.queryOf(0).get('status')).toBe('APPROVED')

    await appeals.resetFilters()
    expect(harness.queryOf(harness.count() - 1).has('status')).toBe(false)
  })

  it('失败时清空队列并保留可读错误', async () => {
    harness = installFetch([failResponse({ status: 503, code: 3, userMessage: '数据库暂不可用' })])
    await appeals.load()
    expect(appeals.items).toEqual([])
    expect(appeals.error).toBe('数据库暂不可用')
  })
})

describe('申诉审核（A-04 第 9 步）', () => {
  it('审核通过 POST 到 /admin/appeals/{id}/approve，不携带幂等键（服务端幂等）', async () => {
    harness = installFetch([
      okResponse({ ...GO_APPEAL, status: 'APPROVED', decidedAt: '2026-09-02T14:00:00Z' })
    ])
    appeals.items = [{ ...GO_APPEAL, createdAt: 1788350400 }]
    await expect(appeals.approve(appeals.items[0])).resolves.toBe(true)

    const index = harness.indexOf('POST', '/admin/appeals/1/approve')
    expect(index).toBe(0)
    expect(harness.headersOf(0)['Idempotency-Key']).toBeUndefined()
    expect(appeals.items[0].status).toBe('APPROVED')
    expect(appeals.pendingCount).toBe(0)
    expect(appeals.notice).toContain('已审核通过')
  })

  it('审核失败时保留可读错误', async () => {
    harness = installFetch([failResponse({ status: 404, code: 4, userMessage: '申诉不存在' })])
    appeals.items = [{ ...GO_APPEAL, createdAt: 1788350400 }]
    await expect(appeals.approve(appeals.items[0])).resolves.toBe(false)
    expect(appeals.error).toBe('申诉不存在')
  })
})
