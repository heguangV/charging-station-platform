import { api } from './http'

/** 活动流程接口（接口文档 §8.1、§8.2）。 */

/** §8.1 活动流程列表；参数 status、stationId、chargerId、userId、page、pageSize。 */
export function fetchFlows(params = {}) {
  return api.get('/admin/flows', params)
}

/** §8.2 强制释放：仅状态 20/30 可用，必须二次确认、填写原因并提交当前流程版本。 */
export function forceReleaseFlow(flowNo, { reason, nextChargerStatus, flowVersion }, { idempotencyKey } = {}) {
  return api.post(
    `/admin/flows/${encodeURIComponent(flowNo)}/force-releases`,
    { confirm: true, reason, nextChargerStatus, flowVersion },
    { idempotent: true, idempotencyKey }
  )
}
