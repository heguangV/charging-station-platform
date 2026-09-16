import { api } from './http'
import { flattenPage, isoToUnixSecond, mapOrder, mapReview } from './contract'
import { fetchOrder } from './charging'

/** 订单列表、小票、评价与申诉接口（Go 契约）。 */

/** 我的订单；status 直接使用 Go 订单状态枚举（CREATED/CHARGING/COMPLETED/...）。 */
export function fetchOrders({ status, page, pageSize } = {}) {
  return api.get('/orders', { status, page, pageSize }).then(flattenPage).then(mapOrderPage)
}

/**
 * 订单详情（GET /orders/{orderNo}）＝ 小票的数据源，也是充电中"估算依据"的来源：
 * 详情比列表多带 startedAt / chargerPowerWatt / unitPriceCentPerKwh 三个只读字段。
 */
export function fetchOrderDetail(orderNo) {
  return fetchOrder(orderNo)
}

/** 订单小票：与订单详情同一个端点，命名保留给结算后的展示路径。 */
export function fetchOrderReceipt(orderNo) {
  return fetchOrderDetail(orderNo)
}

/**
 * 查询本人订单评价。旧契约未评价时返回 {review: null}；
 * Go 契约未评价返回 404，由调用方按 status 404 归一化为无评价。
 */
export function fetchOrderReview(orderNo) {
  return api.get(`/orders/${encodeURIComponent(orderNo)}/review`).then(mapReview)
}

/**
 * 确认支付（UC-U-09）：结算 COMPLETED 且 paymentStatus=PENDING 的订单，从钱包扣款；
 * 余额不足时扣至零、差额记为欠费（PARTIAL_PAID）。未确认的订单会阻止新的充电流程，
 * 因此这是充电闭环的必经一步，不是可选操作。
 */
export function confirmOrder(orderNo, idempotencyKey) {
  return api
    .post(`/orders/${encodeURIComponent(orderNo)}/confirm`, {}, { idempotent: true, idempotencyKey })
    .then(mapOrder)
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

/**
 * 本人查看该订单的申诉（GET /orders/{orderNo}/appeal）。没有申诉时 Go 返回 404，
 * 这里归一化为 null —— 与评价查询同一套处理方式。
 */
export function fetchOrderAppeal(orderNo) {
  return api
    .get(`/orders/${encodeURIComponent(orderNo)}/appeal`)
    .then(raw => ({ ...raw, createdAt: isoToUnixSecond(raw.createdAt), decidedAt: raw.decidedAt ? isoToUnixSecond(raw.decidedAt) : null }))
    .catch(error => {
      if (error?.status === 404) return null
      throw error
    })
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
