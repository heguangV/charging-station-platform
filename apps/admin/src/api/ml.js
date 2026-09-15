import { api } from './http'

/**
 * 预测与 ML 任务接口。
 *
 * §9.1 GET /admin/predictions —— 查询预测结果（只读）。
 * §9.2 POST /admin/ml-tasks —— 启动训练或预测任务。
 * §9.3 GET /admin/ml-tasks/{taskNo} —— 查询任务状态。
 *
 * 注意：接口文档与现有服务端都**没有**提供 “GET /admin/ml-tasks” 列表接口（服务端只有
 * POST /admin/ml-tasks 与 GET /admin/ml-tasks/{taskNo}），因此管理端不臆造该接口，
 * 任务状态只通过任务编号轮询，任务编号由启动接口返回。
 */

/** §9.1 预测查询；参数 stationId、horizonHour=1|6|24、fromAt。 */
export function fetchPredictions(params = {}) {
  return api.get('/admin/predictions', params)
}

/** §9.2 启动 ML 任务：taskType=TRAIN|PREDICT；PREDICT 必须给出 1/6/24 中的周期。 */
export function startMlTask({ taskType, horizonHours }, { idempotencyKey } = {}) {
  const body = { taskType }
  if (Array.isArray(horizonHours) && horizonHours.length > 0) body.horizonHours = horizonHours
  return api.post('/admin/ml-tasks', body, { idempotent: true, idempotencyKey })
}

/** §9.3 查询 ML 任务状态。 */
export function fetchMlTask(taskNo) {
  return api.get(`/admin/ml-tasks/${encodeURIComponent(taskNo)}`)
}
