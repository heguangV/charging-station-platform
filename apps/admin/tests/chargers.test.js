import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useChargersStore, COMMAND_POLL_INTERVAL_MS } from '../src/stores/chargers'
import { failResponse, installFetch, okResponse } from './helpers'

/** Go Charger 契约字段（type 为 AC/DC 字符串，status 为字符串枚举）。 */
const GO_CHARGER = {
  id: 11,
  stationId: 1,
  code: 'ZGC-DC-01',
  type: 'DC',
  powerWatt: 120000,
  status: 'IDLE'
}

/** 适配层归一化后的行形状（旧数字状态码 + statusText + version 恒 0）。 */
const MAPPED_ROW = {
  id: 11,
  stationId: 1,
  code: 'ZGC-DC-01',
  chargerType: 1,
  powerWatt: 120000,
  connectorStandard: '',
  status: 0,
  statusText: '空闲',
  version: 0
}

function goPage(items, total = items.length) {
  return { items, meta: { page: 1, pageSize: 20, total } }
}

let chargers = null
let harness = null

beforeEach(() => {
  setActivePinia(createPinia())
  sessionStorage.clear()
  localStorage.clear()
  chargers = useChargersStore()
})

afterEach(() => {
  chargers.stopPolling()
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

describe('设备列表查询', () => {
  it('按站点/状态组装查询参数；状态数字码映射为 Go 字符串枚举', async () => {
    harness = installFetch([okResponse(goPage([GO_CHARGER]))])
    await chargers.setFilter({ stationId: 1, status: 0 })

    const query = harness.queryOf(0)
    expect(harness.urlOf(0).startsWith('/api/v1/admin/chargers')).toBe(true)
    expect(query.get('stationId')).toBe('1')
    expect(query.get('status')).toBe('IDLE')
    // Go 契约不支持 keyword/chargerType 过滤：不提交，避免伪装成已生效的筛选。
    expect(query.has('keyword')).toBe(false)
    expect(query.has('chargerType')).toBe(false)
    // 响应归一化：AC/DC → 0/1，字符串状态 → 数字码 + statusText，version 恒 0。
    expect(chargers.items[0].status).toBe(0)
    expect(chargers.items[0].statusText).toBe('空闲')
    expect(chargers.items[0].chargerType).toBe(1)
    expect(chargers.items[0].version).toBe(0)
  })

  it('状态 0（空闲）不会被当成“未提供”而丢失', async () => {
    harness = installFetch([okResponse(goPage([]))])
    chargers.filters = { stationId: null, status: 0 }
    await chargers.load()
    expect(harness.queryOf(0).get('status')).toBe('IDLE')
  })

  it('失败时清空列表并给出可读错误', async () => {
    harness = installFetch([failResponse({ status: 503, code: 3, userMessage: '数据库暂不可用' })])
    await chargers.load()
    expect(chargers.error).toBe('数据库暂不可用')
    expect(chargers.items).toEqual([])
    expect(chargers.isEmpty).toBe(false)
  })
})

describe('设备状态变更（Go 契约：PUT status，仅 IDLE/DISABLED）', () => {
  it('提交 status/reason 与幂等键，成功后就地更新该行', async () => {
    harness = installFetch([okResponse({ chargerId: 11, chargerCode: 'ZGC-DC-01', status: 'DISABLED' })])
    chargers.items = [{ ...MAPPED_ROW }]
    await expect(chargers.setStatus(chargers.items[0], 3, '人工巡检离线停用')).resolves.toBe(true)

    expect(harness.indexOf('PUT', '/admin/chargers/11/status')).toBe(0)
    // Go 契约 body 字段为 status（IDLE/DISABLED），无版本乐观锁。
    expect(harness.bodyOf(0)).toEqual({ status: 'DISABLED', reason: '人工巡检离线停用' })
    expect(harness.headersOf(0)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(chargers.items[0].status).toBe(3)
    expect(harness.count()).toBe(1)
  })

  it('目标状态 2（故障）不在 Go 契约允许范围内：显式失败且不发起请求', async () => {
    harness = installFetch([])
    chargers.items = [{ ...MAPPED_ROW }]
    await expect(chargers.setStatus(chargers.items[0], 2, '标记故障')).resolves.toBe(false)
    expect(chargers.error).toContain('IDLE')
    expect(harness.count()).toBe(0)
  })

  it('批量创建设备显式失败，不发起任何请求', async () => {
    harness = installFetch([])
    const ok = await chargers.batchCreate({ stationId: 1, chargers: [{ code: 'X-01', chargerType: 0, powerWatt: 7000 }] })
    expect(ok).toBe(false)
    expect(chargers.error).toContain('暂未提供')
    expect(harness.count()).toBe(0)
  })
})

describe('远程重启与命令查询（Go 契约，标识为 commandId）', () => {
  it('创建重启命令后按 commandId 轮询：404 视为等待回执，回执到达进入终态', async () => {
    vi.useFakeTimers()
    harness = installFetch([
      okResponse({ commandId: 'CMD202609160001', status: 'PENDING' }),
      // 第一轮轮询：回执未到达，Go 返回 404 —— 按"仍在等待"处理。
      failResponse({ status: 404, code: 4, userMessage: '命令不存在或尚未产生回执' }),
      // 第二轮轮询：网关回执已记录。
      okResponse({ commandId: 'CMD202609160001', chargerId: 11, action: 'RESTART', result: 'COMPLETED', applied: true, recordedAt: '2026-09-16T12:00:05Z' })
    ])

    chargers.items = [{ ...MAPPED_ROW }]
    await expect(chargers.restart(chargers.items[0], '远程恢复测试')).resolves.toBe(true)

    expect(harness.indexOf('POST', '/admin/chargers/11/restart')).toBe(0)
    // Go 契约不需要 confirm 字段（二次确认由前端承担），仅提交原因。
    expect(harness.bodyOf(0)).toEqual({ reason: '远程恢复测试' })
    expect(harness.headersOf(0)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(chargers.command.commandId).toBe('CMD202609160001')
    expect(chargers.commandPolling).toBe(true)

    await vi.advanceTimersByTimeAsync(COMMAND_POLL_INTERVAL_MS)
    expect(harness.indexOf('GET', '/admin/device-commands/CMD202609160001')).toBe(1)
    // 404 = 回执未到，命令保持受理态，不写入错误信息。
    expect(chargers.command.status).toBe('PENDING')
    expect(chargers.command.errorSummary).toBe('')

    await vi.advanceTimersByTimeAsync(COMMAND_POLL_INTERVAL_MS)
    expect(chargers.command.status).toBe('SUCCEEDED')
    expect(chargers.commandFinished).toBe(true)
    expect(chargers.commandPolling).toBe(false)
    // 终态后自动刷新设备列表
    expect(harness.indexOf('GET', '/admin/chargers')).toBeGreaterThan(0)
    vi.useRealTimers()
  })

  it('回执报告 FAILED 时命令进入失败态', async () => {
    vi.useFakeTimers()
    harness = installFetch([
      okResponse({ commandId: 'CMD-2', status: 'PENDING' }),
      okResponse({ commandId: 'CMD-2', chargerId: 11, action: 'RESTART', result: 'FAILED', applied: false, recordedAt: '2026-09-16T12:00:06Z' })
    ])
    chargers.items = [{ ...MAPPED_ROW }]
    await chargers.restart(chargers.items[0], '远程恢复测试')
    await vi.advanceTimersByTimeAsync(COMMAND_POLL_INTERVAL_MS)
    expect(chargers.command.status).toBe('FAILED')
    expect(chargers.commandTone).toBe('danger')
    vi.useRealTimers()
  })

  it('重启失败时保留可读错误', async () => {
    harness = installFetch([failResponse({ status: 409, code: 15, userMessage: '当前状态不允许重启' })])
    chargers.items = [{ ...MAPPED_ROW }]
    await expect(chargers.restart(chargers.items[0], '远程恢复测试')).resolves.toBe(false)
    expect(chargers.error).toBe('当前状态不允许重启')
  })
})
