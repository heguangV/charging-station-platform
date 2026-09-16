/**
 * 共享枚举与显示文案。
 *
 * 服务端同时返回整数 status 与 statusText（接口文档 §1.6），代码分支只依赖整数，
 * 文案仅用于筛选器等无法从响应里取到标签的地方，取值与 SRS BR-12 一致。
 */

/** 充电桩 5 种状态：0 空闲、1 在用、2 故障、3 已停用、4 重启中。 */
export const CHARGER_STATUS = [
  { value: 0, key: 'idle', label: '空闲', tone: 'ok' },
  { value: 1, key: 'occupied', label: '在用', tone: 'info' },
  { value: 2, key: 'faulty', label: '故障', tone: 'danger' },
  { value: 3, key: 'disabled', label: '已停用', tone: 'muted' },
  { value: 4, key: 'restarting', label: '重启中', tone: 'warn' }
]

/** 可通过 PUT /chargers/{id}/status 直接设置的目标状态（Go 契约只接受 IDLE/DISABLED；故障设备置空闲即恢复）。 */
export const CHARGER_SETTABLE_STATUS = [0, 3]

/** 充电桩类型：0 交流慢充、1 直流快充。 */
export const CHARGER_TYPES = [
  { value: 0, label: '交流慢充' },
  { value: 1, label: '直流快充' }
]

/** 活动流程状态（§1.6）：未完成/待恢复 10~80，终态 60/70/90。 */
export const FLOW_STATUS = [
  { value: 10, label: '待确认' },
  { value: 20, label: '已预约' },
  { value: 30, label: '待启动' },
  { value: 40, label: '充电中' },
  { value: 50, label: '待结算' },
  { value: 60, label: '已完成' },
  { value: 70, label: '已取消' },
  { value: 80, label: '待恢复' },
  { value: 90, label: '已强制释放' }
]

/** 强制释放仅允许 20/30 两个状态（§8.2）。 */
export const FLOW_RELEASABLE_STATUS = [20, 30]

/** 用户状态：1 正常、0 冻结。 */
export const USER_STATUS = [
  { value: 1, label: '正常', tone: 'ok' },
  { value: 0, label: '冻结', tone: 'danger' }
]

/** 管理员账号状态：1 启用、0 停用。 */
export const ADMIN_STATUS = [
  { value: 1, label: '启用', tone: 'ok' },
  { value: 0, label: '停用', tone: 'danger' }
]

/** 管理员角色取值。 */
export const ADMIN_ROLES = [
  { value: 'OWNER', label: '所有者 OWNER' },
  { value: 'OPERATOR', label: '运营 OPERATOR' },
  { value: 'VIEWER', label: '查看 VIEWER' }
]

/** 站点运营状态（列表接口用 status=0/1 过滤）。 */
export const STATION_STATUS = [
  { value: 1, label: '运营中', tone: 'ok' },
  { value: 0, label: '已停用', tone: 'muted' }
]

/** 服务费调整来源（§7.13）。 */
export const ADJUSTMENT_SOURCES = [
  { value: 'ML_APPROVED', label: '模型建议（ML_APPROVED）' },
  { value: 'MANUAL', label: '人工调整（MANUAL）' }
]

/** 设备命令状态（§7.10）。 */
export const COMMAND_STATUS = {
  PENDING: { label: '待执行', tone: 'warn', terminal: false },
  RUNNING: { label: '执行中', tone: 'info', terminal: false },
  SUCCEEDED: { label: '已成功', tone: 'ok', terminal: true },
  FAILED: { label: '已失败', tone: 'danger', terminal: true }
}

/** ML 任务状态（§9.3）。 */
export const ML_TASK_STATUS = {
  PENDING: { label: '排队中', tone: 'warn', terminal: false },
  RUNNING: { label: '运行中', tone: 'info', terminal: false },
  SUCCEEDED: { label: '已完成', tone: 'ok', terminal: true },
  FAILED: { label: '失败', tone: 'danger', terminal: true },
  TIMED_OUT: { label: '超时', tone: 'danger', terminal: true }
}

/** 预测周期（§9.1）。 */
export const PREDICTION_HORIZONS = [
  { value: 1, label: '未来 1 小时' },
  { value: 6, label: '未来 6 小时' },
  { value: 24, label: '未来 24 小时' }
]

/** 状态文本 → 语义色调：优先用服务端 statusText，找不到时回落为中性。 */
export function toneOfStatusText(text) {
  const label = typeof text === 'string' ? text : ''
  if (['正常', '运营中', '启用', '已完成', '已成功', '有效'].includes(label)) return 'ok'
  if (['冻结', '故障', '已失败', '停用', '超时'].includes(label)) return 'danger'
  if (['充电中', '在用', '运行中', '执行中'].includes(label)) return 'info'
  if (['重启中', '待确认', '已预约', '待启动', '待结算', '待恢复', '排队中'].includes(label)) return 'warn'
  return 'muted'
}
