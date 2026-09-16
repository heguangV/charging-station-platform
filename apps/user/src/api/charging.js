import { api } from './http'
import { flattenPage, mapOrder, isActiveOrder } from './contract'

/**
 * 订单生命周期与钱包接口（Go 契约）。
 *
 * Go 的充电模型用 CREATED 订单承载预约：
 * 创建订单即绑定具体设备并保留 15 分钟，用户在充电页明确确认后才发送 START，
 * START/STOP 是 202 异步设备命令，结算由设备回执驱动，
 * 结束充电 = POST stop 后轮询订单至 COMPLETED 再取小票。
 * 所有业务写入都必须携带 Idempotency-Key（超时重试复用同一键）。
 */

/** 预约设备：按具体 chargerId 创建 CREATED 订单并返回 reservedUntil。 */
export function createOrder({ chargerId }, idempotencyKey) {
  return api.post('/orders', { chargerId }, { idempotent: true, idempotencyKey }).then(mapOrder)
}

/** 当前活动订单：Go 无专用端点，取最近订单列表中第一个未终态的订单。 */
export async function fetchActiveOrder() {
  const data = await api.get('/orders', { page: 1, pageSize: 50 }).then(flattenPage)
  const items = Array.isArray(data.items) ? data.items : []
  const active = items.find(item => isActiveOrder(item.status))
  return active ? mapOrder(active) : null
}

/** 订单详情（也是小票的数据源）。 */
export function fetchOrder(orderNo) {
  return api.get(`/orders/${encodeURIComponent(orderNo)}`).then(mapOrder)
}

/** 取消订单：Go 仅允许尚未开始的订单取消。 */
export function cancelOrder(orderNo, idempotencyKey) {
  return api.post(`/orders/${encodeURIComponent(orderNo)}/cancel`, {}, { idempotent: true, idempotencyKey }).then(mapOrder)
}

/** 开始充电：202 表示命令已受理，设备回执后才进入 CHARGING。 */
export function startOrder(orderNo, idempotencyKey) {
  return api.post(`/orders/${encodeURIComponent(orderNo)}/start`, {}, { idempotent: true, idempotencyKey }).then(mapOrder)
}

/** 停止充电：202 受理后由回执驱动结算，调用方轮询订单终态。 */
export function stopOrder(orderNo, idempotencyKey) {
  return api.post(`/orders/${encodeURIComponent(orderNo)}/stop`, {}, { idempotent: true, idempotencyKey }).then(mapOrder)
}

/** 钱包概览（整数分）。Go 契约没有欠费模型。 */
export function fetchWallet() {
  return api.get('/wallet').then(data => ({ ...data, debtCent: 0, availableCent: data.balanceCent ?? 0 }))
}

/** 虚拟充值，amountCent 范围 1～1,000,000 分。 */
export function rechargeWallet(amountCent, idempotencyKey) {
  return api.post('/wallet/top-up', { amountCent }, { idempotent: true, idempotencyKey })
}

/** 钱包流水。 */
export function fetchWalletTransactions({ type, page, pageSize } = {}) {
  return api.get('/wallet/transactions', { type, page, pageSize }).then(flattenPage)
}
