import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useChargersStore, COMMAND_POLL_INTERVAL_MS, COMMAND_POLL_MAX_ATTEMPTS } from '../src/stores/chargers'
import { failResponse, flush, installFetch, okResponse } from './helpers'

const CHARGER_ROW = {
  id: 11,
  stationId: 1,
  code: 'ZGC-DC-01',
  chargerType: 1,
  powerWatt: 120000,
  connectorStandard: 'GB/T 20234.3',
  status: 0,
  statusText: '空闲',
  totalCount: 4,
  totalMinutes: 130,
  version: 7
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
  it('按站点/状态/类型/关键词组装查询参数', async () => {
    harness = installFetch([okResponse({ items: [CHARGER_ROW], total: 1, page: 1, pageSize: 20 })])
    await chargers.setFilter({ stationId: 1, status: 0, chargerType: 1, keyword: 'ZGC' })

    const query = harness.queryOf(0)
    expect(harness.urlOf(0).startsWith('/api/v1/admin/chargers')).toBe(true)
    expect(query.get('stationId')).toBe('1')
    expect(query.get('status')).toBe('0')
    expect(query.get('chargerType')).toBe('1')
    expect(query.get('keyword')).toBe('ZGC')
  })

  it('状态 0（空闲）不会被当成“未提供”而丢失', async () => {
    harness = installFetch([okResponse({ items: [], total: 0, page: 1, pageSize: 20 })])
    chargers.filters = { stationId: null, status: 0, chargerType: 0, keyword: '' }
    await chargers.load()
    const query = harness.queryOf(0)
    expect(query.get('status')).toBe('0')
    expect(query.get('chargerType')).toBe('0')
    expect(query.has('keyword')).toBe(false)
  })

  it('失败时清空列表并给出可读错误', async () => {
    harness = installFetch([failResponse({ status: 503, code: 3, userMessage: '数据库暂不可用' })])
    await chargers.load()
    expect(chargers.error).toBe('数据库暂不可用')
    expect(chargers.items).toEqual([])
    expect(chargers.isEmpty).toBe(false)
  })
})

describe('设备状态变更', () => {
  it('提交 targetStatus/reason/version 与幂等键，成功后就地更新该行', async () => {
    harness = installFetch([okResponse({ ...CHARGER_ROW, status: 2, statusText: '故障', version: 8 })])
    chargers.items = [{ ...CHARGER_ROW }]
    await expect(chargers.setStatus(chargers.items[0], 2, '人工巡检发现故障')).resolves.toBe(true)

    expect(harness.indexOf('PUT', '/admin/chargers/11/status')).toBe(0)
    expect(harness.bodyOf(0)).toEqual({ targetStatus: 2, reason: '人工巡检发现故障', version: 7 })
    expect(harness.headersOf(0)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(chargers.items[0].status).toBe(2)
    expect(chargers.items[0].statusText).toBe('故障')
    expect(chargers.items[0].version).toBe(8)
    expect(harness.count()).toBe(1)
  })

  it('版本冲突时重载最新状态', async () => {
    harness = installFetch([
      failResponse({ status: 409, code: 22, userMessage: '版本落后' }),
      okResponse({ items: [{ ...CHARGER_ROW, version: 9 }], total: 1, page: 1, pageSize: 20 })
    ])
    chargers.items = [{ ...CHARGER_ROW }]
    await expect(chargers.setStatus(chargers.items[0], 3, '离线停用')).resolves.toBe(false)
    expect(chargers.conflict).toContain('其他管理员')
    expect(harness.indexOf('GET', '/admin/chargers')).toBe(1)
    expect(chargers.items[0].version).toBe(9)
  })
})

describe('批量创建设备', () => {
  it('提交站点与设备数组，携带幂等键并重载列表', async () => {
    harness = installFetch([
      okResponse({ created: ['ZGC-DC-11', 'ZGC-DC-12'] }),
      okResponse({ items: [CHARGER_ROW], total: 1, page: 1, pageSize: 20 })
    ])
    const ok = await chargers.batchCreate({
      stationId: 1,
      chargers: [
        { code: 'ZGC-DC-11', chargerType: 1, powerWatt: 120000, connectorStandard: 'GB/T 20234.3' },
        { code: 'ZGC-DC-12', chargerType: 1, powerWatt: 120000, connectorStandard: 'GB/T 20234.3' }
      ]
    })

    expect(ok).toBe(true)
    const index = harness.indexOf('POST', '/admin/chargers/batch')
    expect(index).toBe(0)
    expect(harness.bodyOf(0).chargers).toHaveLength(2)
    expect(harness.headersOf(0)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(harness.indexOf('GET', '/admin/chargers')).toBe(1)
    expect(chargers.notice).toBe('已创建设备 2 台')
  })

  it('批量创建失败时回显服务端 userMessage，不重载列表', async () => {
    harness = installFetch([failResponse({ status: 422, code: 2, userMessage: '批量创建设备请求内容不符合要求' })])
    const ok = await chargers.batchCreate({ stationId: 1, chargers: [{ code: 'X-01', chargerType: 0, powerWatt: 7000 }] })
    expect(ok).toBe(false)
    expect(chargers.error).toBe('批量创建设备请求内容不符合要求')
    expect(harness.count()).toBe(1)
  })
})

describe('远程重启与命令轮询', () => {
  it('创建重启命令后进入重启中，并轮询到终态', async () => {
    vi.useFakeTimers()
    harness = installFetch([
      okResponse({ commandNo: 'CMD202609020001', status: 'PENDING', chargerStatus: 4, createdAt: 1788235200 }),
      okResponse({ commandNo: 'CMD202609020001', status: 'RUNNING', chargerId: 11, chargerCode: 'ZGC-DC-01', createdAt: 1788235200, completedAt: null }),
      okResponse({ commandNo: 'CMD202609020001', status: 'SUCCEEDED', chargerId: 11, chargerCode: 'ZGC-DC-01', createdAt: 1788235200, completedAt: 1788235205 }),
      okResponse({ items: [{ ...CHARGER_ROW, status: 0, statusText: '空闲' }], total: 1, page: 1, pageSize: 20 })
    ])

    chargers.items = [{ ...CHARGER_ROW }]
    await expect(chargers.restart(chargers.items[0], '远程恢复测试')).resolves.toBe(true)

    const createIndex = harness.indexOf('POST', '/admin/chargers/11/restart-commands')
    expect(createIndex).toBe(0)
    expect(harness.bodyOf(0)).toEqual({ confirm: true, reason: '远程恢复测试' })
    expect(harness.headersOf(0)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(chargers.command.commandNo).toBe('CMD202609020001')
    // 服务端返回 chargerStatus=4，设备立即进入“重启中”
    expect(chargers.items[0].status).toBe(4)
    expect(chargers.commandPolling).toBe(true)

    await vi.advanceTimersByTimeAsync(COMMAND_POLL_INTERVAL_MS)
    expect(harness.indexOf('GET', '/admin/device-commands/CMD202609020001')).toBe(1)
    expect(chargers.command.status).toBe('RUNNING')
    expect(chargers.commandFinished).toBe(false)

    await vi.advanceTimersByTimeAsync(COMMAND_POLL_INTERVAL_MS)
    expect(chargers.command.status).toBe('SUCCEEDED')
    expect(chargers.commandFinished).toBe(true)
    expect(chargers.commandPolling).toBe(false)
    // 终态后自动刷新设备列表
    expect(harness.indexOf('GET', '/admin/chargers')).toBeGreaterThan(0)
  })

  it('轮询达到上限后停止并提示稍后刷新', async () => {
    vi.useFakeTimers()
    harness = installFetch([
      okResponse({ commandNo: 'CMD-2', status: 'PENDING', chargerStatus: 4, createdAt: 1788235200 }),
      okResponse({ commandNo: 'CMD-2', status: 'RUNNING', chargerId: 11, chargerCode: 'ZGC-DC-01', createdAt: 1788235200 })
    ])
    chargers.items = [{ ...CHARGER_ROW }]
    await chargers.restart(chargers.items[0], '远程恢复测试')

    await vi.advanceTimersByTimeAsync(COMMAND_POLL_INTERVAL_MS * (COMMAND_POLL_MAX_ATTEMPTS + 2))
    expect(chargers.commandPolling).toBe(false)
    expect(chargers.commandAttempts).toBe(COMMAND_POLL_MAX_ATTEMPTS)
    expect(chargers.notice).toContain('仍在处理中')
  })

  it('命令状态查询失败时保留命令并记录安全错误信息', async () => {
    vi.useFakeTimers()
    harness = installFetch([
      okResponse({ commandNo: 'CMD-3', status: 'PENDING', chargerStatus: 4, createdAt: 1788235200 }),
      okResponse({
        commandNo: 'CMD-3',
        status: 'FAILED',
        chargerId: 11,
        chargerCode: 'ZGC-DC-01',
        createdAt: 1788235200,
        completedAt: 1788235206,
        errorSummary: '设备通信超时'
      }),
      okResponse({ items: [{ ...CHARGER_ROW }], total: 1, page: 1, pageSize: 20 })
    ])
    chargers.items = [{ ...CHARGER_ROW }]
    await chargers.restart(chargers.items[0], '远程恢复测试')
    await flush()
    await vi.advanceTimersByTimeAsync(COMMAND_POLL_INTERVAL_MS)

    expect(chargers.command.status).toBe('FAILED')
    expect(chargers.command.errorSummary).toBe('设备通信超时')
    expect(chargers.commandTone).toBe('danger')
  })
})
