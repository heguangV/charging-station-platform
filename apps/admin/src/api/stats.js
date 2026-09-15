import { api, STATS_TIMEOUT_MS } from './http'
import { toInteger, toNumber } from '../utils/format'
import { CHARGER_STATUS } from '../utils/domain'

/**
 * 统计接口（接口文档 §8.3、§8.4）。
 *
 * 服务端返回的统计响应字段全部是整数（分、毫瓦时、台数、百分比），
 * 这里在进入 store 之前做一次显式解析：字段缺失或类型不对时抛出 DtoError，
 * 由页面展示“统计响应字段不完整”，绝不把 NaN 渲染到 KPI 或图表上。
 */

/** 响应体结构不符合契约时抛出的错误，携带可直接展示的 userMessage。 */
export class DtoError extends Error {
  constructor(message) {
    super(message)
    this.name = 'DtoError'
    this.userMessage = message
  }
}

function requireInteger(value, field) {
  const parsed = toInteger(value)
  if (parsed === null) throw new DtoError(`${field} 字段缺失或格式不正确`)
  return parsed
}

function requireNumber(value, field) {
  const parsed = toNumber(value)
  if (parsed === null) throw new DtoError(`${field} 字段缺失或格式不正确`)
  return parsed
}

/**
 * §8.3 营收统计。
 * @param {object} params fromAt、toAt、stationId、bucket=day|hour（时间范围最大 90 天）。
 */
export function fetchRevenueStats(params = {}) {
  return api.get('/admin/stats/revenue', params, { timeout: STATS_TIMEOUT_MS })
}

/** 解析 §8.3 响应：items 与三个总计字段都必须存在且为整数。 */
export function parseRevenueStats(data) {
  if (!data || typeof data !== 'object') throw new DtoError('营收统计响应为空')
  if (!Array.isArray(data.items)) throw new DtoError('营收统计响应缺少 items 列表')
  const items = data.items.map((item, index) => {
    if (!item || typeof item !== 'object') throw new DtoError(`营收统计第 ${index + 1} 条记录格式不正确`)
    return {
      bucketStart: requireInteger(item.bucketStart, `items[${index}].bucketStart`),
      amountCent: requireInteger(item.amountCent, `items[${index}].amountCent`),
      energyMwh: requireInteger(item.energyMwh, `items[${index}].energyMwh`),
      orderCount: requireInteger(item.orderCount, `items[${index}].orderCount`)
    }
  })
  return {
    items,
    totalAmountCent: requireInteger(data.totalAmountCent, 'totalAmountCent'),
    totalEnergyMwh: requireInteger(data.totalEnergyMwh, 'totalEnergyMwh'),
    totalOrderCount: requireInteger(data.totalOrderCount, 'totalOrderCount')
  }
}

/**
 * §8.4 设备状态统计。
 * @param {object} params stationId 可选。
 */
export function fetchChargerStatusStats(params = {}) {
  return api.get('/admin/stats/charger-status', params, { timeout: STATS_TIMEOUT_MS })
}

/** 解析 §8.4 响应：五种状态数量、可运营数、总数与健康度都必须存在。 */
export function parseChargerStatusStats(data) {
  if (!data || typeof data !== 'object') throw new DtoError('设备状态统计响应为空')
  const stats = {
    idleCount: requireInteger(data.idleCount, 'idleCount'),
    occupiedCount: requireInteger(data.occupiedCount, 'occupiedCount'),
    faultyCount: requireInteger(data.faultyCount, 'faultyCount'),
    restartingCount: requireInteger(data.restartingCount, 'restartingCount'),
    disabledCount: requireInteger(data.disabledCount, 'disabledCount'),
    operationalCount: requireInteger(data.operationalCount, 'operationalCount'),
    totalCount: requireInteger(data.totalCount, 'totalCount'),
    healthPercent: requireNumber(data.healthPercent, 'healthPercent')
  }
  if (stats.totalCount < 0 || stats.operationalCount < 0) throw new DtoError('设备状态统计数量不能为负数')
  return stats
}

/** 空统计：接口未返回数据时保持页面结构稳定，而非渲染 NaN。 */
export function emptyChargerStatusStats() {
  return {
    idleCount: 0,
    occupiedCount: 0,
    faultyCount: 0,
    restartingCount: 0,
    disabledCount: 0,
    operationalCount: 0,
    totalCount: 0,
    healthPercent: 0
  }
}

/** 把统计拆成图表用的分段（顺序固定为 空闲/在用/故障/已停用/重启中）。 */
export function chargerStatusBreakdown(stats) {
  if (!stats) return []
  const counts = {
    idle: stats.idleCount,
    occupied: stats.occupiedCount,
    faulty: stats.faultyCount,
    disabled: stats.disabledCount,
    restarting: stats.restartingCount
  }
  return CHARGER_STATUS.map(item => ({
    key: item.key,
    label: item.label,
    tone: item.tone,
    value: toInteger(counts[item.key]) ?? 0
  }))
}
