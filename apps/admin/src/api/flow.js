import { api } from './http'
import { flattenPage, mapOrderToFlow, legacyToOrderStatus } from './contract'

/**
 * 活动流程接口（Go 契约）。
 * Go 模型是"订单"：列表即 /admin/orders，flowNo 即订单号；
 * 强制释放按设备执行：POST /admin/chargers/{chargerId}/release。
 */

/** 流程（订单）列表；Go 契约仅支持 orderNo/status 过滤 + 分页。 */
export function fetchFlows(params = {}) {
  const { status, orderNo, page, pageSize } = params
  return api
    .get('/admin/orders', {
      orderNo,
      status: status === undefined || status === null ? undefined : legacyToOrderStatus(status),
      page,
      pageSize
    })
    .then(flattenPage)
    .then(data => ({ ...data, items: (Array.isArray(data.items) ? data.items : []).map(mapOrderToFlow) }))
}

/**
 * 强制释放（UC-U-07/管理链路）：按 chargerId 执行，Go 要求 reason 与
 * targetStatus（IDLE/DISABLED），202 语义下的同步释放接口返回释放后的设备状态。
 */
export function forceReleaseFlow(chargerId, { reason, nextChargerStatus }, { idempotencyKey } = {}) {
  return api.post(
    `/admin/chargers/${encodeURIComponent(chargerId)}/release`,
    { reason, targetStatus: nextChargerStatus === 3 ? 'DISABLED' : 'IDLE' },
    { idempotent: true, idempotencyKey }
  )
}
