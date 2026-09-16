import { AGENT_TIMEOUT_MS, api } from './http'

/**
 * AI 助手会话接口。
 * 前端绝不直接调用 LLM 或腾讯地图 WebService：所有工具编排、Key 与降级策略都在服务端，
 * 因此这里只发一次只读 POST，且不携带 Idempotency-Key（接口文档 §1.2：查询与只读请求除外）。
 */
export function chatWithAgent({ message, location, coordinateType, chargerType } = {}) {
  const payload = { message }
  if (location && Number.isInteger(location.latitudeE6) && Number.isInteger(location.longitudeE6)) {
    payload.location = { latitudeE6: location.latitudeE6, longitudeE6: location.longitudeE6 }
    // 默认 gcj02；浏览器定位得到的 WGS-84 坐标必须显式声明。
    payload.coordinateType = coordinateType || 'gcj02'
  }
  if (chargerType === 0 || chargerType === 1) payload.chargerType = chargerType
  return api.post('/user/agent/chat', payload, { timeout: AGENT_TIMEOUT_MS })
}
