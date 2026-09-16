import { api } from './http'
import { flattenPage, mapOrder, mapReview } from './contract'
import { fetchOrder } from './charging'

/** 订单列表、小票、评价与申诉接口（Go 契约）。 */

/** 我的订单；status 直接使用 Go 订单状态枚举（CREATED/CHARGING/COMPLETED/...）。 */
export function fetchOrders({ status, page, pageSize } = {}) {
  return api.get('/orders', { status, page, pageSize }).then(flattenPage).then(mapOrderPage)
}

/** 订单小票（Go 契约字段：价格快照、电量 Wh、应付、实付、支付状态）。 */
export function fetchOrderReceipt(orderNo) {
  return fetchOrder(orderNo)
}

/**
 * 查询本人订单评价。旧契约未评价时返回 {review: null}；
 * Go 契约未评价返回 404，由调用方按 status 404 归一化为无评价。
 */
export function fetchOrderReview(orderNo) {
  return api.get(`/orders/${encodeURIComponent(orderNo)}/review`).then(mapReview)
}

/** 提交评价：Go 字段为 stars/comment（1..5 星、1..500 码点），同内容重放幂等返回首次结果。 */
export function submitOrderReview(orderNo, { rating, content }, idempotencyKey) {
  return api
    .post(
      `/orders/${encodeURIComponent(orderNo)}/review`,
      { stars: rating, comment: content },
      { idempotent: true, idempotencyKey }
    )
    .then(mapReview)
}

/** 提交申诉（UC-U-09）：仅本人 COMPLETED 订单；同内容重放幂等，不同内容 409。 */
export function createAppeal(orderNo, { reason }, idempotencyKey) {
  return api.post(
    `/orders/${encodeURIComponent(orderNo)}/appeal`,
    { reason },
    { idempotent: true, idempotencyKey }
  )
}

function mapOrderPage(data) {
  return { ...data, items: (Array.isArray(data.items) ? data.items : []).map(mapOrder) }
}
