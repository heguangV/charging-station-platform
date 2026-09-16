import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import ChargersView from '../src/views/ChargersView.vue'
import { vReveal } from '../src/directives/reveal'
import { useChargersStore, COMMAND_POLL_MAX_ATTEMPTS, COMMAND_POLL_INTERVAL_MS } from '../src/stores/chargers'
import { formatDateTime } from '../src/utils/format'
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
let pinia = null

beforeEach(() => {
  pinia = createPinia()
  setActivePinia(pinia)
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

  it('批量建桩走 POST /admin/chargers/batch，桩型翻译为 AC/DC 且不带接口标准', async () => {
    harness = installFetch([
      okResponse({
        stationId: 1,
        chargerCount: 2,
        created: [
          { id: 11, stationId: 1, code: 'ZGC-DC-11', type: 'DC', powerWatt: 120000, status: 'IDLE' },
          { id: 12, stationId: 1, code: 'ZGC-DC-12', type: 'DC', powerWatt: 120000, status: 'IDLE' }
        ]
      }),
      okResponse(goPage([]))
    ])

    const ok = await chargers.batchCreate({
      stationId: 1,
      chargers: [
        { code: 'ZGC-DC-11', chargerType: 1, powerWatt: 120000, connectorStandard: 'GB/T 20234.3' },
        { code: 'ZGC-DC-12', chargerType: 1, powerWatt: 120000 }
      ]
    })

    expect(ok).toBe(true)
    const index = harness.indexOf('POST', '/admin/chargers/batch')
    expect(index).toBe(0)
    // 旧数字桩型（1=直流）翻译成契约用词；接口标准在 Go 契约里没有对应列，不发送。
    expect(harness.bodyOf(index)).toEqual({
      stationId: 1,
      chargers: [
        { code: 'ZGC-DC-11', connectorType: 'DC', powerWatt: 120000 },
        { code: 'ZGC-DC-12', connectorType: 'DC', powerWatt: 120000 }
      ]
    })
    expect(harness.headersOf(index)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(chargers.notice).toContain('2')
  })

  it('交流桩型翻译为 AC', async () => {
    harness = installFetch([okResponse({ stationId: 1, chargerCount: 1, created: [] }), okResponse(goPage([]))])
    await chargers.batchCreate({
      stationId: 1,
      chargers: [{ code: 'ZGC-AC-01', chargerType: 0, powerWatt: 7000 }]
    })
    expect(harness.bodyOf(0).chargers[0].connectorType).toBe('AC')
  })

  it('编号已存在（409 ALREADY_EXISTS）时保留错误提示且不改动列表', async () => {
    harness = installFetch([failResponse({ status: 409, code: 5, userMessage: '编号 ZGC-DC-11 已存在' })])
    chargers.items = [{ ...MAPPED_ROW }]

    await expect(
      chargers.batchCreate({ stationId: 1, chargers: [{ code: 'ZGC-DC-11', chargerType: 1, powerWatt: 120000 }] })
    ).resolves.toBe(false)

    expect(chargers.error).toContain('已存在')
    expect(chargers.items).toHaveLength(1)
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
      // 回执时间戳带毫秒（Go 的 RFC3339 就是这样），曾经被 Date.parse(...)/1000 + toInteger
      // 判成"非整数"而丢掉，面板因此永远显示"待执行"。用例必须用真实形态的时间戳。
      okResponse({ commandId: 'CMD202609160001', chargerId: 11, action: 'RESTART', result: 'COMPLETED', applied: true, recordedAt: '2026-09-16T12:00:05.123456789Z' })
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
    // 回执时间落成整数秒（毫秒向下取整），面板才能显示完成时间而不是"待执行"
    expect(chargers.command.completedAt).toBe(Date.UTC(2026, 8, 16, 12, 0, 5) / 1000)
    expect(formatDateTime(chargers.command.completedAt)).toMatch(/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}$/)
    // 受理响应没有受理时间字段：不伪造 0（否则页面渲染"提交于 —"）
    expect(chargers.command.createdAt).toBeUndefined()
    // 终态后自动刷新设备列表
    expect(harness.indexOf('GET', '/admin/chargers')).toBeGreaterThan(0)
    vi.useRealTimers()
  })

  it('回执报告 FAILED 时命令进入失败态', async () => {
    vi.useFakeTimers()
    harness = installFetch([
      okResponse({ commandId: 'CMD-2', status: 'PENDING' }),
      okResponse({ commandId: 'CMD-2', chargerId: 11, action: 'RESTART', result: 'FAILED', applied: false, recordedAt: '2026-09-16T12:00:06.987654321Z' })
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

describe('重启命令面板', () => {
  it('等待回执到达轮询上限时提示真实 commandId', async () => {
    vi.useFakeTimers()
    chargers.command = { commandId: 'CMD-WAIT-01', status: 'PENDING' }
    vi.spyOn(chargers, 'pollCommand').mockResolvedValue(chargers.command)
    chargers.startPolling()
    await vi.advanceTimersByTimeAsync(COMMAND_POLL_INTERVAL_MS * COMMAND_POLL_MAX_ATTEMPTS)
    expect(chargers.commandPolling).toBe(false)
    expect(chargers.notice).toContain('CMD-WAIT-01')
    expect(chargers.notice).not.toContain('undefined')
  })

  it('回执到达后显示完成时间，且不渲染契约里没有的受理时间', async () => {
    window.matchMedia = vi.fn(() => ({
      matches: false,
      media: '(prefers-reduced-motion: reduce)',
      addEventListener: () => {},
      removeEventListener: () => {},
      addListener: () => {},
      removeListener: () => {}
    }))
    installFetch([okResponse({ items: [], meta: { page: 1, pageSize: 20, total: 0 } })])

    // 回执时间戳（毫秒已向下取整为整数秒），来自 DeviceCommand.recordedAt
    const completedAt = Date.UTC(2026, 8, 16, 12, 0, 5) / 1000
    chargers.command = {
      commandId: 'CMD202609160001',
      status: 'SUCCEEDED',
      chargerStatus: null,
      completedAt,
      errorSummary: '',
      chargerCode: 'ZGC-DC-01'
    }

    const wrapper = mount(ChargersView, {
      global: { plugins: [pinia], directives: { reveal: vReveal } }
    })
    await flushPromises()

    const panel = wrapper.get('[data-testid="charger-command-panel"]').text()
    expect(panel).toContain('CMD202609160001')
    // 完成时间必须落地：之前 completedAt 被判成非整数而丢掉，这里永远显示"待执行"
    expect(panel).toContain(formatDateTime(completedAt))
    expect(panel).not.toContain('待执行')
    // 受理响应没有受理时间字段，面板不该出现"提交于 —"
    expect(panel).not.toContain('提交于')
    delete window.matchMedia
  })
})
