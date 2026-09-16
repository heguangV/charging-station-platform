import { toInteger, toNumber } from '../utils/format'
import { CHARGER_STATUS } from '../utils/domain'

/**
 * 统计接口：Go 契约暂无 /admin/stats/*（A-07 统计模块，待立项）。
 * 两个 fetch 显式抛出不可用；本文件的 DTO 解析助手保留，
 * 供 dashboard/Overview 在拿到合法结构前维持空状态与格式约束。
 */

/** 响应体结构不符合契约时抛出的错误，携带可直接展示的 userMessage。 */
export class DtoError extends Error {
  constructor(message) {
    super(message)
    this.name = 'DtoError'
    this.userMessage = message
  }
}

/** 营收统计与设备状态统计在 Go 后端暂未提供。 */
export function fetchRevenueStats() {
  return Promise.reject(new DtoError('营收统计在 Go 后端暂未提供（A-07 统计待立项）'))
}

export function fetchChargerStatusStats() {
  return Promise.reject(new DtoError('设备状态统计在 Go 后端暂未提供（A-07 统计待立项）'))
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

/** 解析营收统计响应：items 与三个总计字段都必须存在且为整数。 */
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

/** 解析设备状态统计响应：五种状态数量、可运营数、总数与健康度都必须存在。 */
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
