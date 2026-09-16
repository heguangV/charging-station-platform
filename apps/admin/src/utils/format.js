/**
 * 显示层格式化工具。
 *
 * 单位约定（数据库设计 §2）：金额一律整数分、电量整数毫瓦时、时间 UTC Unix 秒。
 * 只有这一层做换算与本地化；接口层与 store 永远只传整数，避免浮点误差进入业务判断。
 *
 * 关键约束：任何无法解析的输入都返回占位符 '—'，绝不把 NaN / undefined 渲染到界面上。
 */

/** 空值占位符：所有格式化函数的兜底输出。 */
export const EMPTY = '—'

/** 数值判定：只接受有限数值（含可安全解析的数字字符串），其余一律视为缺失。 */
export function toNumber(value) {
  if (typeof value === 'number') return Number.isFinite(value) ? value : null
  if (typeof value === 'string' && value.trim() !== '') {
    const parsed = Number(value)
    return Number.isFinite(parsed) ? parsed : null
  }
  return null
}

/** 整数判定：分、毫瓦时、秒都必须是有限整数。 */
export function toInteger(value) {
  const parsed = toNumber(value)
  return parsed === null || !Number.isInteger(parsed) ? null : parsed
}

/** 分 → 元（数值），缺失返回 null。 */
export function centToYuan(cent) {
  const value = toInteger(cent)
  return value === null ? null : value / 100
}

/** 分 → 元（文本），例如 1250 → '12.50'。 */
export function formatAmount(cent, { digits = 2, withSymbol = false } = {}) {
  const yuan = centToYuan(cent)
  if (yuan === null) return EMPTY
  const text = yuan.toFixed(digits)
  return withSymbol ? `¥${text}` : text
}

/** 毫瓦时 → 千瓦时（数值）：1 kWh = 1_000_000 mWh。 */
export function mwhToKwh(mwh) {
  const value = toInteger(mwh)
  return value === null ? null : value / 1000000
}

/** 毫瓦时 → 千瓦时（文本），例如 1500000 → '1.50'。 */
export function formatEnergy(mwh, { digits = 2, withUnit = false } = {}) {
  const kwh = mwhToKwh(mwh)
  if (kwh === null) return EMPTY
  const text = kwh.toFixed(digits)
  return withUnit ? `${text} kWh` : text
}

/** 瓦 → 千瓦（文本），例如 60000 → '60.0'。 */
export function formatPower(watt, { digits = 1, withUnit = true } = {}) {
  const value = toInteger(watt)
  if (value === null) return EMPTY
  const text = (value / 1000).toFixed(digits)
  return withUnit ? `${text} kW` : text
}

/** 百分比：只做四舍五入与拼接，缺失返回占位符。 */
export function formatPercent(value, { digits = 1, withSign = false } = {}) {
  const parsed = toNumber(value)
  if (parsed === null) return EMPTY
  const text = `${parsed.toFixed(digits)}%`
  return withSign && parsed > 0 ? `+${text}` : text
}

/** 整数（订单量、台数）文本。 */
export function formatInt(value) {
  const parsed = toInteger(value)
  return parsed === null ? EMPTY : String(parsed)
}

/** 分钟 → '1.5 小时'；不足 1 小时显示分钟。 */
export function formatMinutes(minutes) {
  const parsed = toInteger(minutes)
  if (parsed === null) return EMPTY
  return parsed >= 60 ? `${(parsed / 60).toFixed(1)} 小时` : `${parsed} 分钟`
}

/** 字节 → 人类可读（备份体积）。 */
export function formatBytes(bytes) {
  const parsed = toInteger(bytes)
  if (parsed === null) return EMPTY
  if (parsed < 1024) return `${parsed} B`
  const units = ['KB', 'MB', 'GB', 'TB']
  let size = parsed / 1024
  let index = 0
  while (size >= 1024 && index < units.length - 1) {
    size /= 1024
    index += 1
  }
  return `${size.toFixed(1)} ${units[index]}`
}

function toDate(seconds) {
  const value = toInteger(seconds)
  if (value === null || value <= 0) return null
  const date = new Date(value * 1000)
  return Number.isNaN(date.getTime()) ? null : date
}

function pad(value) {
  return String(value).padStart(2, '0')
}

/** UTC 秒 → 本地 'YYYY-MM-DD'。 */
export function formatDate(seconds) {
  const date = toDate(seconds)
  if (!date) return EMPTY
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}`
}

/** UTC 秒 → 本地 'YYYY-MM-DD HH:mm'。 */
export function formatDateTime(seconds) {
  const date = toDate(seconds)
  if (!date) return EMPTY
  return `${formatDate(seconds)} ${pad(date.getHours())}:${pad(date.getMinutes())}`
}

/** UTC 秒 → 本地 'MM-DD'，用于趋势图的横轴。 */
export function formatDayLabel(seconds) {
  const date = toDate(seconds)
  if (!date) return EMPTY
  return `${pad(date.getMonth() + 1)}-${pad(date.getDate())}`
}

/** UTC 秒 → 本地 'MM-DD HH:mm'，用于小时粒度。 */
export function formatHourLabel(seconds) {
  const date = toDate(seconds)
  if (!date) return EMPTY
  return `${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${pad(date.getHours())}:00`
}

/** 当前时间的 UTC 秒。 */
export function nowSeconds() {
  return Math.floor(Date.now() / 1000)
}

/** 本地自然日 00:00:00 对应的 UTC 秒。 */
export function startOfLocalDay(seconds) {
  const date = toDate(seconds)
  if (!date) return null
  date.setHours(0, 0, 0, 0)
  return Math.floor(date.getTime() / 1000)
}

/** 本地自然日 23:59:59 对应的 UTC 秒。 */
export function endOfLocalDay(seconds) {
  const start = startOfLocalDay(seconds)
  return start === null ? null : start + 86399
}

/** 最近 N 天（含今天）的起始 UTC 秒。 */
export function secondsOfRecentDays(days) {
  const count = toInteger(days)
  if (count === null || count <= 0) return null
  const start = startOfLocalDay(nowSeconds())
  return start === null ? null : start - (count - 1) * 86400
}

/** <input type="datetime-local"> 的值 → UTC 秒；空值返回 null。 */
export function fromDateTimeInputValue(value) {
  if (typeof value !== 'string' || value === '') return null
  const timestamp = new Date(value).getTime()
  return Number.isNaN(timestamp) ? null : Math.floor(timestamp / 1000)
}
