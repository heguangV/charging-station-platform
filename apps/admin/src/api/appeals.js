import { api } from './http'
import { flattenPage, isoToUnixSecond } from './contract'

/** 管理端申诉接口（Go 契约，A-04 第 8/9 步：申诉队列与审核）。 */

/** 申诉队列；status 过滤 PENDING/APPROVED，按创建时间倒序分页。 */
export function fetchAppeals({ status, page, pageSize } = {}) {
  return api.get('/admin/appeals', { status, page, pageSize }).then(flattenPage).then(mapAppealPage)
}

/**
 * 审核通过（UC-U-09 pt 5）：申诉转 APPROVED、订单取消、实付金额退回钱包并写审计。
 * Go 契约的重复审核是幂等无操作，直接返回当前结果，因此不携带 Idempotency-Key。
 */
export function approveAppeal(appealId) {
  return api.post(`/admin/appeals/${encodeURIComponent(appealId)}/approve`).then(mapAppeal)
}

/**
 * 审核驳回：申诉转 REJECTED 并记录处理意见，**订单与钱包都不动**——退款与取消订单是"通过"才做的事。
 * 契约保证重复决策是无操作且不改变已有结论，因此同样不带幂等键。
 */
export function rejectAppeal(appealId, reason) {
  return api.post(`/admin/appeals/${encodeURIComponent(appealId)}/reject`, { reason }).then(mapAppeal)
}

function mapAppealPage(data) {
  return { ...data, items: (Array.isArray(data.items) ? data.items : []).map(mapAppeal) }
}

function mapAppeal(raw) {
  if (!raw || typeof raw !== 'object') return null
  return {
    ...raw,
    createdAt: isoToUnixSecond(raw.createdAt),
    decidedAt: raw.decidedAt ? isoToUnixSecond(raw.decidedAt) : null
  }
}
