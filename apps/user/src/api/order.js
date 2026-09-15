import { api } from './http'

/** 订单与评价接口（接口文档 §3.4、§3.5、§5.9）。 */

/** §3.4 我的订单；sort 仅允许 -createdAt / createdAt。 */
export function fetchOrders({ status, fromAt, toAt, page, pageSize, sort } = {}) {
  return api.get('/user/orders', { status, fromAt, toAt, page, pageSize, sort })
}

/** §3.5 订单小票（完整价格快照、时长、电量、应付、实付、欠费）。 */
export function fetchOrderReceipt(orderNo) {
  return api.get(`/user/orders/${encodeURIComponent(orderNo)}`)
}

/** §5.9 查询本人订单评价，未评价时返回 {review: null}。 */
export function fetchOrderReview(orderNo) {
  return api.get(`/user/orders/${encodeURIComponent(orderNo)}/review`)
}

/** §5.9 提交评价：rating 必填 1～5，content 必填 1～500 码点。 */
export function submitOrderReview(orderNo, { rating, content }, idempotencyKey) {
  return api.post(`/user/orders/${encodeURIComponent(orderNo)}/review`, { rating, content }, { idempotent: true, idempotencyKey })
}
