import { api } from './http'

/** 充电流程与钱包接口（接口文档 §3、§5）。所有写入都必须携带 Idempotency-Key。 */

/** §5.1 请求充电或入队。 */
export function requestFlow({ stationId, chargerType = 1, preferredChargerId = null }, idempotencyKey) {
  return api.post('/user/flows', { stationId, chargerType, preferredChargerId }, { idempotent: true, idempotencyKey })
}

/** §5.2 当前活动流程；登录、重连和刷新后恢复页面的唯一入口。 */
export function fetchActiveFlow() {
  return api.get('/user/flows/active')
}

/** §5.3 流程详情（含待确认报价快照）。 */
export function fetchFlow(flowNo) {
  return api.get(`/user/flows/${encodeURIComponent(flowNo)}`)
}

/** §5.4 确认报价并预约。 */
export function confirmQuote(flowNo, { quoteNo, flowVersion }, idempotencyKey) {
  return api.post(`/user/flows/${encodeURIComponent(flowNo)}/quote-confirmations`, { quoteNo, flowVersion }, { idempotent: true, idempotencyKey })
}

/** §5.5 取消流程。 */
export function cancelFlow(flowNo, { reasonCode = 'USER_CANCELLED', flowVersion }, idempotencyKey) {
  return api.post(`/user/flows/${encodeURIComponent(flowNo)}/cancellations`, { reasonCode, flowVersion }, { idempotent: true, idempotencyKey })
}

/** §5.6 开始充电。 */
export function startFlow(flowNo, { flowVersion, targetAmountCent = null, balanceFloorCent = null }, idempotencyKey) {
  return api.post(
    `/user/flows/${encodeURIComponent(flowNo)}/start`,
    { flowVersion, targetAmountCent, balanceFloorCent },
    { idempotent: true, idempotencyKey }
  )
}

/** §5.7 充电进度；服务端按快照计算，客户端按秒轮询即可。 */
export function fetchProgress(flowNo) {
  return api.get(`/user/flows/${encodeURIComponent(flowNo)}/progress`)
}

/** §5.8 结束充电并结算，返回 SettlementReceipt。 */
export function settleFlow(flowNo, { flowVersion, reasonCode = 'USER_STOPPED' }, idempotencyKey) {
  return api.post(`/user/flows/${encodeURIComponent(flowNo)}/settlements`, { flowVersion, reasonCode }, { idempotent: true, idempotencyKey })
}

/** §3.1 钱包概览（金额均为整数分）。 */
export function fetchWallet() {
  return api.get('/user/wallet')
}

/** §3.2 虚拟充值，amountCent 范围 1～1,000,000 分。 */
export function rechargeWallet(amountCent, idempotencyKey) {
  return api.post('/user/wallet/recharges', { amountCent }, { idempotent: true, idempotencyKey })
}

/** §3.3 钱包流水。 */
export function fetchWalletTransactions({ type, fromAt, toAt, page, pageSize } = {}) {
  return api.get('/user/wallet/transactions', { type, fromAt, toAt, page, pageSize })
}
