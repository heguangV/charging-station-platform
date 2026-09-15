import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useFlowsStore } from '../src/stores/flows'
import { failResponse, installFetch, okResponse } from './helpers'

const FLOW_ROW = {
  flowNo: 'FLOW202609020001',
  userId: 7,
  stationId: 1,
  chargerId: 11,
  chargerCode: 'ZGC-DC-01',
  chargerType: 1,
  status: 20,
  statusText: '已预约',
  version: 3,
  createdAt: 1788235200
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

describe('活动流程查询', () => {
  it('数字过滤参数只在为正整数时提交，空字符串按未提供处理', async () => {
    harness = installFetch([okResponse({ items: [FLOW_ROW], total: 1, page: 1, pageSize: 20 })])
    await flows.setFilter({ status: 20, stationId: '1', chargerId: '11', userId: '' })

    const query = harness.queryOf(0)
    expect(harness.urlOf(0).startsWith('/api/v1/admin/flows')).toBe(true)
    expect(query.get('status')).toBe('20')
    expect(query.get('stationId')).toBe('1')
    expect(query.get('chargerId')).toBe('11')
    expect(query.has('userId')).toBe(false)
  })

  it('非法数字过滤（0 或非数字）不下发', async () => {
    harness = installFetch([okResponse({ items: [], total: 0, page: 1, pageSize: 20 })])
    flows.filters = { status: null, stationId: '0', chargerId: 'abc', userId: '-3' }
    await flows.load()
    const query = harness.queryOf(0)
    expect(query.has('stationId')).toBe(false)
    expect(query.has('chargerId')).toBe(false)
    expect(query.has('userId')).toBe(false)
  })

  it('empty / 失败 / 加载中三种状态', async () => {
    harness = installFetch([okResponse({ items: [], total: 0, page: 1, pageSize: 20 })])
    const pending = flows.load()
    expect(flows.loading).toBe(true)
    await pending
    expect(flows.isEmpty).toBe(true)

    harness.always(failResponse({ status: 422, code: 2, userMessage: '流程过滤参数不符合要求' }))
    await flows.load()
    expect(flows.error).toBe('流程过滤参数不符合要求')
  })

  it('只有状态 20/30 允许强制释放', async () => {
    flows.items = [FLOW_ROW, { ...FLOW_ROW, flowNo: 'FLOW-2', status: 40, statusText: '充电中' }]
    expect(flows.canRelease(flows.items[0])).toBe(true)
    expect(flows.canRelease(flows.items[1])).toBe(false)
    expect(flows.releasableCount).toBe(1)
  })
})

describe('强制释放', () => {
  it('提交 confirm/reason/nextChargerStatus/flowVersion 与幂等键，并就地更新该行', async () => {
    harness = installFetch([okResponse({ flowNo: FLOW_ROW.flowNo, status: 90, statusText: '已强制释放', version: 4 })])
    flows.items = [{ ...FLOW_ROW }]
    await expect(
      flows.forceRelease(flows.items[0], { reason: '设备计划维护', nextChargerStatus: 2 })
    ).resolves.toBe(true)

    const index = harness.indexOf('POST', `/admin/flows/${FLOW_ROW.flowNo}/force-releases`)
    expect(index).toBe(0)
    expect(harness.bodyOf(0)).toEqual({
      confirm: true,
      reason: '设备计划维护',
      nextChargerStatus: 2,
      flowVersion: 3
    })
    expect(harness.headersOf(0)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(flows.items[0].status).toBe(90)
    expect(flows.items[0].statusText).toBe('已强制释放')
    expect(flows.items[0].version).toBe(4)
    expect(flows.notice).toContain('已强制释放')
  })

  it('版本冲突时提示并重载列表', async () => {
    harness = installFetch([
      failResponse({ status: 409, code: 22, userMessage: '版本落后' }),
      okResponse({ items: [{ ...FLOW_ROW, version: 6 }], total: 1, page: 1, pageSize: 20 })
    ])
    flows.items = [{ ...FLOW_ROW }]
    await expect(flows.forceRelease(flows.items[0], { reason: '设备计划维护', nextChargerStatus: 2 })).resolves.toBe(false)
    expect(flows.conflict).toContain('其他管理员')
    expect(harness.indexOf('GET', '/admin/flows')).toBe(1)
    expect(flows.items[0].version).toBe(6)
  })

  it('非法状态迁移时明确提示改用受控结算流程', async () => {
    harness = installFetch([
      failResponse({ status: 409, code: 15, userMessage: '当前状态不允许该操作' }),
      okResponse({ items: [], total: 0, page: 1, pageSize: 20 })
    ])
    flows.items = [{ ...FLOW_ROW }]
    await expect(flows.forceRelease(flows.items[0], { reason: '设备计划维护', nextChargerStatus: 0 })).resolves.toBe(false)
    expect(flows.conflict).toContain('充电中请使用设备重启的受控结算流程')
    expect(flows.items).toEqual([])
  })
})
