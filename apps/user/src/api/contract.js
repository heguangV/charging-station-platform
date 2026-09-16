/**
 * Go 契约适配层（A-02 方言切换）。
 *
 * Go 后端（develop api/openapi.yaml）与旧 C++ 契约的差异集中在这里消化：
 * 订单字符串状态 ↔ 旧数字流程码、ISO 时间 ↔ Unix 秒、瓦时 ↔ 毫瓦时、
 * 设备 AC/DC ↔ 0/1、分页 meta 展平。store 与视图继续消费既有形状，
 * 本模块是唯一知道 Go 契约字段细节的地方。
 */

/** 旧 UI 桩型码：0 交流慢充、1 直流快充（Go 契约为 AC/DC 字符串）。 */
export function connectorTypeToLegacy(type) {
  return type === 'DC' ? 1 : 0
}

export function legacyChargerTypeToConnector(chargerType) {
  return chargerType === 1 ? 'DC' : 'AC'
}

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

/** 订单状态 → 旧流程状态码（仅用于既有分支/筛选；文案一律走 statusText）。 */
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
  CREATED: '已预约',
  STARTING: '启动中',
  CHARGING: '充电中',
  STOPPING: '停止中',
  COMPLETED: '已完成',
  CANCELLED: '已取消',
  FAILED: '已失败',
  EXPIRED: '已过期'
}

/** 仍占用设备或等待终态确认的订单状态。 */
export const ACTIVE_ORDER_STATUSES = ['CREATED', 'STARTING', 'CHARGING', 'STOPPING']

export function isActiveOrder(status) {
  return ACTIVE_ORDER_STATUSES.includes(status)
}

/** ISO 8601 → Unix 秒；Go 契约只传 ISO 时间，展示层统一消费秒。 */
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

/** Go 订单 → 前端展示形状。电量 Wh→毫瓦时，时间 ISO→秒，时长取创建到更新的差值。 */
export function mapOrder(raw) {
  if (!raw || typeof raw !== 'object') return null
  const status = typeof raw.status === 'string' ? raw.status : ''
  const createdAt = isoToUnixSecond(raw.createdAt)
  const updatedAt = isoToUnixSecond(raw.updatedAt)
  return {
    ...raw,
    statusText: ORDER_STATUS_LABELS[status] || status,
    statusCode: ORDER_STATUS_TO_LEGACY[status] ?? null,
    active: isActiveOrder(status),
    stationName: raw.stationName || `站点 ${raw.stationId ?? '—'}`,
    chargerCode: raw.chargerCode || (raw.chargerId != null ? `#${raw.chargerId}` : ''),
    reservedUntil: raw.reservedUntil ? isoToUnixSecond(raw.reservedUntil) : null,
    // 契约在详情里给出真实开始时间（设备 CHARGE_STARTED 的事实时间）；列表没有该字段，
    // 旧行为是拿 createdAt 顶上——只在缺少真实值时兜底，否则充电时长会从下单时刻算起。
    startedAt: raw.startedAt ? isoToUnixSecond(raw.startedAt) : createdAt,
    settledAt: status === 'COMPLETED' ? updatedAt : 0,
    durationSec: updatedAt > createdAt ? updatedAt - createdAt : 0,
    energyMwh: Number.isFinite(raw.energyWh) ? raw.energyWh * 1000 : null,
    // 充电中的实时计量（设备 CHARGE_PROGRESS）：与结算值分开，页面上优先显示它。
    meteredEnergyMwh: Number.isFinite(raw.meteredEnergyWh) ? raw.meteredEnergyWh * 1000 : null,
    meteredAmountCent: Number.isFinite(raw.meteredAmountCent) ? raw.meteredAmountCent : null,
    meteredAt: raw.meteredAt ? isoToUnixSecond(raw.meteredAt) : null
  }
}

/** Go 评价 → 旧字段名（rating/content）。 */
export function mapReview(raw) {
  if (!raw || typeof raw !== 'object') return null
  return {
    ...raw,
    rating: raw.stars,
    content: raw.comment,
    createdAt: isoToUnixSecond(raw.createdAt)
  }
}

/** Go 评论墙条目 → 旧字段名。 */
export function mapWallEntry(raw) {
  if (!raw || typeof raw !== 'object') return null
  return {
    ...raw,
    rating: raw.stars,
    content: raw.comment,
    createdAt: isoToUnixSecond(raw.createdAt)
  }
}

/** Go 身份 → 旧 user 形状（昵称/手机号脱敏/头像/注册时间）。 */
export function mapIdentity(identity) {
  if (!identity || typeof identity !== 'object') return null
  return {
    id: identity.id,
    role: identity.role,
    adminRole: identity.adminRole,
    status: identity.status,
    displayName: identity.displayName,
    nickname: identity.displayName,
    username: identity.displayName
  }
}

/** Go /me/profile（ProfileView）→ 旧 user 形状补充字段。 */
export function mapProfileView(profile) {
  if (!profile || typeof profile !== 'object') return null
  return {
    id: profile.id,
    displayName: profile.displayName,
    nickname: profile.displayName,
    username: profile.displayName,
    phoneMasked: profile.phone || '',
    avatarUrl: profile.avatarUrl || '',
    status: profile.status,
    registeredAt: isoToUnixSecond(profile.registeredAt)
  }
}

/**
 * 聚合资料视图：旧 /user/me 合并视图由 /me + /me/profile + /wallet 组合而成。
 * Go 契约没有欠费与活动流程标记，二者置为无欠费/无活动流程，由对应接口单独查询。
 */
export function composeProfile(identity, profile, wallet) {
  const user = { ...mapIdentity(identity), ...mapProfileView(profile) }
  return {
    user,
    balanceCent: Number.isFinite(wallet?.balanceCent) ? wallet.balanceCent : 0,
    debtCent: 0,
    hasActiveFlow: false,
    version: null
  }
}
