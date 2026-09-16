import { afterEach, describe, expect, it, vi } from 'vitest'
import {
  DtoError,
  chargerStatusBreakdown,
  emptyChargerStatusStats,
  fetchChargerStatusStats,
  fetchRevenueStats,
  parseChargerStatusStats,
  parseRevenueStats
} from '../src/api/stats'
import { installFetch, okResponse } from './helpers'
import {
  EMPTY,
  formatAmount,
  formatBytes,
  formatDateTime,
  formatEnergy,
  formatInt,
  formatMinutes,
  formatPercent,
  formatPower,
  centToYuan,
  mwhToKwh
} from '../src/utils/format'

const REVENUE_DTO = {
  items: [
    { bucketStart: 1788134400, amountCent: 123456, energyMwh: 1500000, orderCount: 12 },
    { bucketStart: 1788220800, amountCent: 0, energyMwh: 0, orderCount: 0 }
  ],
  totalAmountCent: 123456,
  totalEnergyMwh: 1500000,
  totalOrderCount: 12
}

const CHARGER_STATUS_DTO = {
  idleCount: 12,
  occupiedCount: 5,
  faultyCount: 2,
  restartingCount: 1,
  disabledCount: 4,
  operationalCount: 17,
  totalCount: 24,
  healthPercent: 70.83
}

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('营收统计 DTO 解析', () => {
  it('保留整数分与整数毫瓦时，不做隐式换算', () => {
    const parsed = parseRevenueStats(REVENUE_DTO)
    expect(parsed.totalAmountCent).toBe(123456)
    expect(parsed.totalEnergyMwh).toBe(1500000)
    expect(parsed.totalOrderCount).toBe(12)
    expect(parsed.items).toHaveLength(2)
    expect(parsed.items[0]).toEqual({
      bucketStart: 1788134400,
      amountCent: 123456,
      energyMwh: 1500000,
      orderCount: 12
    })
  })

  it('缺少总计或 items 时抛出 DtoError，而不是渲染 NaN', () => {
    expect(() => parseRevenueStats({ items: [] })).toThrow(DtoError)
    expect(() => parseRevenueStats({})).toThrow(/items/)
    expect(() => parseRevenueStats(null)).toThrow(/为空/)
  })

  it('单条记录的字段类型不对时立即失败', () => {
    expect(() =>
      parseRevenueStats({ ...REVENUE_DTO, items: [{ bucketStart: 1, amountCent: 'oops', energyMwh: 0, orderCount: 0 }] })
    ).toThrow(/amountCent/)
    expect(() =>
      parseRevenueStats({ ...REVENUE_DTO, items: [{ bucketStart: null, amountCent: 1, energyMwh: 0, orderCount: 0 }] })
    ).toThrow(/bucketStart/)
  })
})

describe('统计接口请求', () => {
  it('营收统计把区间、站点与桶粒度原样传给 Go 契约', async () => {
    const harness = installFetch([okResponse(REVENUE_DTO)])

    const data = await fetchRevenueStats({ fromAt: 1788134400, toAt: 1788220800, stationId: 3, bucket: 'day' })

    expect(harness.methodOf(0)).toBe('GET')
    expect(harness.urlOf(0)).toBe(
      '/api/v1/admin/stats/revenue?fromAt=1788134400&toAt=1788220800&stationId=3&bucket=day'
    )
    // 请求层不做换算也不做解析：整数分与整数毫瓦时原样返回，由 parseRevenueStats 校验。
    expect(data.totalAmountCent).toBe(123456)
    expect(data.totalEnergyMwh).toBe(1500000)
    expect(parseRevenueStats(data).items).toHaveLength(2)
  })

  it('未选择的站点筛选不会变成 stationId=null 或空串', async () => {
    const harness = installFetch([okResponse(REVENUE_DTO)])

    await fetchRevenueStats({ fromAt: 1788134400, toAt: 1788220800, stationId: null, bucket: 'hour' })

    const query = harness.queryOf(0)
    expect(query.has('stationId')).toBe(false)
    expect(query.get('bucket')).toBe('hour')
  })

  it('设备状态统计请求 /admin/stats/chargers，并按需带上站点', async () => {
    const harness = installFetch([okResponse(CHARGER_STATUS_DTO)])

    const all = await fetchChargerStatusStats()
    expect(harness.urlOf(0)).toBe('/api/v1/admin/stats/chargers')
    expect(parseChargerStatusStats(all).totalCount).toBe(24)

    await fetchChargerStatusStats({ stationId: 7 })
    expect(harness.queryOf(1).get('stationId')).toBe('7')
  })

  it('服务端返回不符合契约的统计时抛出 DtoError，而不是渲染 NaN', async () => {
    installFetch([okResponse({ items: [] })])

    const data = await fetchRevenueStats({ fromAt: 1, toAt: 2, bucket: 'day' })

    expect(() => parseRevenueStats(data)).toThrow(DtoError)
  })
})

describe('设备状态统计 DTO 解析', () => {
  it('解析五种状态、可运营数、总数与健康度', () => {
    const parsed = parseChargerStatusStats(CHARGER_STATUS_DTO)
    expect(parsed.idleCount).toBe(12)
    expect(parsed.occupiedCount).toBe(5)
    expect(parsed.faultyCount).toBe(2)
    expect(parsed.restartingCount).toBe(1)
    expect(parsed.disabledCount).toBe(4)
    expect(parsed.operationalCount).toBe(17)
    expect(parsed.totalCount).toBe(24)
    expect(parsed.healthPercent).toBeCloseTo(70.83, 5)
  })

  it('字段缺失时抛出 DtoError', () => {
    expect(() => parseChargerStatusStats({})).toThrow(/idleCount/)
    expect(() => parseChargerStatusStats({ ...CHARGER_STATUS_DTO, healthPercent: 'NaN' })).toThrow(/healthPercent/)
    expect(() => parseChargerStatusStats({ ...CHARGER_STATUS_DTO, totalCount: -1 })).toThrow(/负数/)
  })

  it('空统计数据是明确的零值，不影响页面结构', () => {
    const empty = emptyChargerStatusStats()
    expect(empty.totalCount).toBe(0)
    expect(empty.healthPercent).toBe(0)
    expect(chargerStatusBreakdown(empty).every(item => item.value === 0)).toBe(true)
  })

  it('状态分段顺序与文案固定为空闲/在用/故障/已停用/重启中', () => {
    const breakdown = chargerStatusBreakdown(parseChargerStatusStats(CHARGER_STATUS_DTO))
    expect(breakdown.map(item => item.label)).toEqual(['空闲', '在用', '故障', '已停用', '重启中'])
    expect(breakdown.map(item => item.value)).toEqual([12, 5, 2, 4, 1])
    expect(breakdown.map(item => item.key)).toEqual(['idle', 'occupied', 'faulty', 'disabled', 'restarting'])
  })
})

describe('展示层单位换算', () => {
  it('整数分 → 元：1250 分显示为 12.50', () => {
    expect(centToYuan(1250)).toBe(12.5)
    expect(formatAmount(1250)).toBe('12.50')
    expect(formatAmount(0)).toBe('0.00')
    expect(formatAmount(-350)).toBe('-3.50')
    expect(formatAmount(123456, { withSymbol: true })).toBe('¥1234.56')
  })

  it('整数毫瓦时 → 千瓦时：1500000 mWh 显示为 1.50 kWh', () => {
    expect(mwhToKwh(1500000)).toBe(1.5)
    expect(formatEnergy(1500000)).toBe('1.50')
    expect(formatEnergy(1500000, { withUnit: true })).toBe('1.50 kWh')
  })

  it('功率、百分比、整数与体积格式化', () => {
    expect(formatPower(60000)).toBe('60.0 kW')
    expect(formatPercent(70.83)).toBe('70.8%')
    expect(formatPercent(70.83, { digits: 2 })).toBe('70.83%')
    expect(formatInt(24)).toBe('24')
    expect(formatMinutes(90)).toBe('1.5 小时')
    expect(formatMinutes(45)).toBe('45 分钟')
    expect(formatBytes(2048)).toBe('2.0 KB')
  })

  it('UTC 秒按本地时间展示，非法值不渲染 NaN', () => {
    expect(formatDateTime(1788134400)).toMatch(/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}$/)
    expect(formatDateTime(null)).toBe(EMPTY)
    expect(formatDateTime(0)).toBe(EMPTY)
  })

  it('非法输入统一回落为占位符，任何格式化结果都不含 NaN', () => {
    const values = [undefined, null, '', 'abc', NaN, Infinity, {}, []]
    for (const value of values) {
      for (const formatter of [formatAmount, formatEnergy, formatPercent, formatInt, formatPower, formatBytes]) {
        const text = formatter(value)
        expect(text).toBe(EMPTY)
        expect(String(text)).not.toContain('NaN')
      }
    }
  })
})
