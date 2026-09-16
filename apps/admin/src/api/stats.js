import { api } from './http'
import { toInteger, toNumber } from '../utils/format'
import { CHARGER_STATUS } from '../utils/domain'

/**
 * 统计接口（Go 契约 GET /admin/stats/revenue 与 /admin/stats/chargers）。
 *
 * 旧 Qt 管理端的统计来自服务端预计算的驾驶舱快照，Go 侧改为按需聚合：
 * 营收按「已完成订单的冻结金额、归属停止充电时刻」统计，分桶对齐请求区间起点，
 * 空区间也会返回零值桶，图表因此不会把安静的日子跳过。
 *
 * 服务端返回整数分与整数毫瓦时；这里只做契约校验，不做任何隐式换算，
 * 单位换算全部留给展示层。
 */

/** 响应体结构不符合契约时抛出的错误，携带可直接展示的 userMessage。 */
export class DtoError extends Error {
  constructor(message) {
    super(message)
    this.name = 'DtoError'
    this.userMessage = message
  }
}

/**
 * 营收统计：fromAt/toAt 为 UTC 秒且必填，单次区间上限 90 天；
 * stationId 可选，bucket 为 day（趋势）或 hour（单日明细）。
 * 解析交给 parseRevenueStats，使 DTO 校验只有一处。
 */
export function fetchRevenueStats({ fromAt, toAt, stationId, bucket } = {}) {
  return api.get('/admin/stats/revenue', { fromAt, toAt, stationId, bucket })
}

/** 设备状态统计；stationId 可选。解析交给 parseChargerStatusStats。 */
export function fetchChargerStatusStats({ stationId } = {}) {
  return api.get('/admin/stats/chargers', { stationId })
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
