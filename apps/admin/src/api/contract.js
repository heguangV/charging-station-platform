/**
 * Go 契约适配层（A-02 方言切换，管理端）。
 *
 * Go 后端（develop api/openapi.yaml）与旧 C++ 契约的差异集中在这里消化：
 * 订单/设备/用户的字符串状态 ↔ 旧数字状态码、ISO 时间 ↔ Unix 秒、分页 meta 展平。
 * store 与视图继续消费既有形状（statusText、version 等展示字段），
 * 本模块是唯一知道 Go 契约字段细节的地方。
 */

/** Go 设备状态 → 旧数字码（0 空闲、1 在用、2 故障、3 已停用、4 重启中）。 */
export const CHARGER_STATUS_TO_LEGACY = {
  IDLE: 0,
  OCCUPIED: 1,
  FAULT: 2,
  DISABLED: 3,
  RESTARTING: 4
}

export const CHARGER_STATUS_LABELS = {
  IDLE: '空闲',
  OCCUPIED: '在用',
  FAULT: '故障',
  DISABLED: '已停用',
  RESTARTING: '重启中'
}

export function mapChargerStatus(status) {
  return CHARGER_STATUS_TO_LEGACY[status] ?? status
}

export function mapChargerStatusText(status) {
  return CHARGER_STATUS_LABELS[status] || status
}

/**
 * 旧数字桩型（0 交流慢充 / 1 直流快充）→ Go 的连接器类型。
 * 契约用 AC/DC 这两个词，与列名、列表载荷和站点筛选参数一致。
 */
export function legacyToConnectorType(value) {
  if (value === 1) return 'DC'
  if (value === 0) return 'AC'
  return undefined
}

/** 旧数字设备状态 → Go 枚举（列表过滤参数）。 */
export function legacyToChargerStatus(value) {
  const map = { 0: 'IDLE', 1: 'OCCUPIED', 2: 'FAULT', 3: 'DISABLED', 4: 'RESTARTING' }
  return map[value] ?? undefined
}

/**
 * Go 订单状态 → 旧流程状态码（仅用于既有分支/筛选，文案一律走 statusText）。
 * CREATED/STARTING 统一映射到 30（待启动），STOPPING 映射到 50（待结算）。
 */
export const ORDER_STATUS_TO_LEGACY = {
  CREATED: 30,
  STARTING: 30,
  CHARGING: 40,
  STOPPING: 50,
  COMPLETED: 60,
  CANCELLED: 70,
  FAILED: 70,
  EXPIRED: 70
}

export const ORDER_STATUS_LABELS = {
  CREATED: '待启动',
  STARTING: '启动中',
  CHARGING: '充电中',
  STOPPING: '停止中',
  COMPLETED: '已完成',
  CANCELLED: '已取消',
  FAILED: '已失败',
  EXPIRED: '已过期'
}

export function mapOrderStatus(status) {
  return ORDER_STATUS_TO_LEGACY[status] ?? status
}

export function mapOrderStatusText(status) {
  return ORDER_STATUS_LABELS[status] || status
}

/** 旧数字流程状态 → Go 枚举（列表过滤参数）；Go 无排队/报价阶段，10/20 映射到 CREATED。 */
export function legacyToOrderStatus(value) {
  const map = { 10: 'CREATED', 20: 'CREATED', 30: 'CREATED', 40: 'CHARGING', 50: 'STOPPING', 60: 'COMPLETED', 70: 'CANCELLED', 90: 'CANCELLED' }
  return map[value] ?? undefined
}

/** Go 用户状态 ACTIVE/DISABLED ↔ 旧数字码（1 正常、0 冻结）。 */
export function mapUserStatus(status) {
  return status === 'ACTIVE' ? 1 : status === 'DISABLED' ? 0 : status
}

export function mapUserStatusText(status) {
  return status === 'ACTIVE' ? '正常' : status === 'DISABLED' ? '冻结' : status || '—'
}

export function legacyToUserStatus(value) {
  const map = { 1: 'ACTIVE', 0: 'DISABLED' }
  return map[value] ?? undefined
}

/** ISO 8601 → Unix 秒。 */
export function isoToUnixSecond(value) {
  const time = Date.parse(value)
  return Number.isFinite(time) ? Math.floor(time / 1000) : 0
}

/** Go 分页 {items, meta:{page,pageSize,total}} → 旧扁平 {items, page, pageSize, total}。 */
export function flattenPage(data) {
  const payload = data && typeof data === 'object' ? data : {}
  const meta = payload.meta && typeof payload.meta === 'object' ? payload.meta : {}
  return {
    ...payload,
    page: Number.isInteger(meta.page) ? meta.page : undefined,
    pageSize: Number.isInteger(meta.pageSize) ? meta.pageSize : undefined,
    total: Number.isInteger(meta.total) ? meta.total : undefined
  }
}

/** Go 身份 → 管理端 profile 快照（角色取 adminRole；Go 契约无 OWNER/改密标记）。 */
export function mapAdminIdentity(identity) {
  if (!identity || typeof identity !== 'object') return null
  const role = identity.adminRole || identity.role
  return {
    id: identity.id,
    username: identity.displayName || identity.username || '',
    displayName: identity.displayName || identity.username || '',
    roles: role ? [role] : [],
    status: identity.status,
    mustChangePassword: false
  }
}

/** Go 订单 → 旧活动流程行（flowNo 即订单号；版本字段 Go 契约没有，恒为 0）。 */
export function mapOrderToFlow(raw) {
  if (!raw || typeof raw !== 'object') return null
  const createdAt = isoToUnixSecond(raw.createdAt)
  const updatedAt = isoToUnixSecond(raw.updatedAt)
  return {
    ...raw,
    flowNo: raw.orderNo,
    status: mapOrderStatus(raw.status),
    statusText: mapOrderStatusText(raw.status),
    version: 0,
    stationName: raw.stationName || `站点 ${raw.stationId ?? '—'}`,
    chargerCode: raw.chargerCode || (raw.chargerId != null ? `#${raw.chargerId}` : ''),
    energyMwh: Number.isFinite(raw.energyWh) ? raw.energyWh * 1000 : null,
    startedAt: createdAt,
    durationSec: updatedAt > createdAt ? updatedAt - createdAt : 0
  }
}

/** Go 设备 → 旧行形状（版本恒为 0；Go 契约无接口标准字段）。 */
export function mapCharger(raw) {
  if (!raw || typeof raw !== 'object') return null
  return {
    ...raw,
    chargerType: raw.type === 'DC' ? 1 : raw.type === 'AC' ? 0 : raw.type,
    status: mapChargerStatus(raw.status),
    statusText: mapChargerStatusText(raw.status),
    version: 0,
    connectorStandard: ''
  }
}

/** Go 用户摘要/详情 → 旧行形状。 */
export function mapUser(raw) {
  if (!raw || typeof raw !== 'object') return null
  return {
    ...raw,
    status: mapUserStatus(raw.status),
    statusText: mapUserStatusText(raw.status),
    version: 0,
    registeredAt: raw.registeredAt ? isoToUnixSecond(raw.registeredAt) : raw.registeredAt
  }
}

/** Go 钱包流水条目（管理端按用户查询）→ 展示形状。 */
export function mapWalletEntry(raw) {
  if (!raw || typeof raw !== 'object') return null
  return {
    ...raw,
    createdAt: isoToUnixSecond(raw.createdAt)
  }
}

/** Go 审计条目 → 展示形状（targetType/targetId 映射到 resource 命名）。 */
export function mapAuditEntry(raw) {
  if (!raw || typeof raw !== 'object') return null
  return {
    ...raw,
    targetType: raw.resourceType,
    targetId: raw.resourceId,
    createdAt: isoToUnixSecond(raw.createdAt)
  }
}
