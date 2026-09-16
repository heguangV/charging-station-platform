import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useFlowsStore } from '../src/stores/flows'
import { failResponse, installFetch, okResponse } from './helpers'

/** Go 订单契约字段（管理端订单列表 = 旧活动流程列表）。 */
const GO_ORDER = {
  orderNo: 'ORD202609020001',
  userId: 7,
  stationId: 1,
  chargerId: 11,
  status: 'CREATED',
  amountCent: 0,
  createdAt: '2026-09-02T12:00:00Z',
  updatedAt: '2026-09-02T12:00:00Z'
}

/** 适配层归一化后的行形状（旧字段名 + 数字状态码）。 */
const FLOW_ROW = {
  flowNo: 'ORD202609020001',
  userId: 7,
  stationId: 1,
  chargerId: 11,
  status: 30,
  statusText: '待启动',
  version: 0,
  createdAt: 1788235200
}

function goPage(items, total = items.length) {
  return { items, meta: { page: 1, pageSize: 20, total } }
}

let flows = null
let harness = null

beforeEach(() => {
  setActivePinia(createPinia())
  sessionStorage.clear()
  localStorage.clear()
  flows = useFlowsStore()
})

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('活动流程查询（Go 契约：/admin/orders）', () => {
  it('只提交 Go 支持的过滤：状态映射为字符串枚举、订单号原文提交', async () => {
    harness = installFetch([okResponse(goPage([GO_ORDER]))])
    await flows.setFilter({ status: 30, orderNo: 'ORD202609020001' })

    const query = harness.queryOf(0)
    expect(harness.urlOf(0).startsWith('/api/v1/admin/orders')).toBe(true)
    expect(query.get('status')).toBe('CREATED')
    expect(query.get('orderNo')).toBe('ORD202609020001')
    // Go 契约不支持 stationId/chargerId/userId 过滤：不提交，避免伪装成已生效的筛选。
    expect(query.has('stationId')).toBe(false)
    expect(query.has('chargerId')).toBe(false)
    expect(query.has('userId')).toBe(false)
    // 响应归一化：订单 → 旧流程行（flowNo 即订单号）。
    expect(flows.items[0].flowNo).toBe('ORD202609020001')
    expect(flows.items[0].status).toBe(30)
    expect(flows.items[0].statusText).toBe('待启动')
  })

  it('归一化把 CREATED/STARTING 映射为待启动（30）', async () => {
    harness = installFetch([
      okResponse(goPage([GO_ORDER, { ...GO_ORDER, orderNo: 'ORD-2', status: 'STARTING' }, { ...GO_ORDER, orderNo: 'ORD-3', status: 'CHARGING' }]))
    ])
    await flows.load()
    expect(flows.items.map(item => item.status)).toEqual([30, 30, 40])
    expect(flows.items.map(item => item.statusText)).toEqual(['待启动', '启动中', '充电中'])
  })

  it('empty / 失败 / 加载中三种状态', async () => {
    harness = installFetch([okResponse(goPage([]))])
    const pending = flows.load()
    expect(flows.loading).toBe(true)
    await pending
    expect(flows.isEmpty).toBe(true)

    harness.always(failResponse({ status: 422, code: 2, userMessage: '流程过滤参数不符合要求' }))
    await flows.load()
    expect(flows.error).toBe('流程过滤参数不符合要求')
  })

  it('只有待启动（30）允许强制释放', async () => {
    flows.items = [FLOW_ROW, { ...FLOW_ROW, flowNo: 'ORD-2', status: 40, statusText: '充电中' }]
    expect(flows.canRelease(flows.items[0])).toBe(true)
    expect(flows.canRelease(flows.items[1])).toBe(false)
    expect(flows.releasableCount).toBe(1)
  })
})

describe('强制释放（Go 契约：按设备释放）', () => {
  it('按 chargerId 提交 reason 与 targetStatus，携带幂等键并就地更新该行', async () => {
    harness = installFetch([okResponse({ chargerId: 11, chargerCode: 'ZGC-DC-01', status: 'IDLE' })])
    flows.items = [{ ...FLOW_ROW }]
    await expect(
      flows.forceRelease(flows.items[0], { reason: '设备计划维护', nextChargerStatus: 0 })
    ).resolves.toBe(true)

    const index = harness.indexOf('POST', '/admin/chargers/11/release')
    expect(index).toBe(0)
    // Go 契约要求 reason 与 targetStatus（IDLE/DISABLED），无版本乐观锁。
    expect(harness.bodyOf(0)).toEqual({ reason: '设备计划维护', targetStatus: 'IDLE' })
    expect(harness.headersOf(0)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(flows.items[0].status).toBe(90)
    expect(flows.items[0].statusText).toBe('已强制释放')
    expect(flows.notice).toContain('已强制释放')
  })

  it('非法状态迁移时明确提示改用受控结算流程', async () => {
    harness = installFetch([
      failResponse({ status: 409, code: 15, userMessage: '当前状态不允许该操作' }),
      okResponse(goPage([]))
    ])
    flows.items = [{ ...FLOW_ROW }]
    await expect(flows.forceRelease(flows.items[0], { reason: '设备计划维护', nextChargerStatus: 0 })).resolves.toBe(false)
    expect(flows.conflict).toContain('充电中请使用设备重启的受控结算流程')
    expect(flows.items).toEqual([])
  })

  it('nextChargerStatus=3 映射为 DISABLED（停用）', async () => {
    harness = installFetch([okResponse({ chargerId: 11, chargerCode: 'ZGC-DC-01', status: 'DISABLED' })])
    flows.items = [{ ...FLOW_ROW }]
    await flows.forceRelease(flows.items[0], { reason: '设备计划维护', nextChargerStatus: 3 })
    expect(harness.bodyOf(0).targetStatus).toBe('DISABLED')
    expect(flows.notice).toContain('已停用')
  })
})
