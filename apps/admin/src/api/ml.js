/**
 * 预测与 ML 任务接口：Go 契约暂无该域（A-07/ML 模块，待立项）。
 * 保留原函数签名并显式抛出不可用，页面据此门控。
 */

export function fetchPredictions() {
  return unsupported('预测查询在 Go 后端暂未提供（A-07/ML 待立项）')
}

export function startMlTask() {
  return unsupported('ML 任务在 Go 后端暂未提供（A-07/ML 待立项）')
}

export function fetchMlTask() {
  return unsupported('ML 任务查询在 Go 后端暂未提供（A-07/ML 待立项）')
}
