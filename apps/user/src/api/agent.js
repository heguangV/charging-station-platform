import { AGENT_TIMEOUT_MS, api } from './http'
import { mapStation } from './station'

/**
 * AI 助手会话接口（Go 契约 POST /agent/chat）。
 *
 * 前端绝不直接调用 LLM 或腾讯地图 WebService：所有工具编排、Server Key 与降级策略
 * 都在服务端，客户端只提交问题、自己的位置与可选的桩型偏好。
 *
 * 服务端永不因模型或地图不可用而失败——它按确定性路径作答并在 `degraded` 里说明，
 * 因此这里只发一次只读 POST，且不携带 Idempotency-Key（接口文档 §1.2：只读请求除外）。
 */
export function chatWithAgent({ message, location, coordinateType, chargerType } = {}) {
  const payload = { message }
  if (location && Number.isInteger(location.latitudeE6) && Number.isInteger(location.longitudeE6)) {
    payload.location = { latitudeE6: location.latitudeE6, longitudeE6: location.longitudeE6 }
    // 默认 gcj02；浏览器定位得到的 WGS-84 坐标必须显式声明，服务端据此转换。
    payload.coordinateType = coordinateType || 'gcj02'
  }
  if (chargerType === 0 || chargerType === 1) payload.chargerType = chargerType
  return api.post('/agent/chat', payload, { timeout: AGENT_TIMEOUT_MS }).then(mapAgentResult)
}

/**
 * 服务端结果 → 组件消费的形状。
 *
 * stations 走与站点列表相同的映射：助手返回的是契约站点字段加价格拆分与桩型构成，
 * 而 StationCard 消费的是展示名，两处必须同源。pois / route / actions / tools 的字段
 * 名与组件一致，原样透传。
 */
export function mapAgentResult(result) {
  const payload = result && typeof result === 'object' ? result : {}
  return {
    ...payload,
    stations: (Array.isArray(payload.stations) ? payload.stations : []).map(mapAgentStation).filter(Boolean),
    pois: Array.isArray(payload.pois) ? payload.pois : [],
    route: payload.route || null,
    actions: Array.isArray(payload.actions) ? payload.actions : [],
    tools: Array.isArray(payload.tools) ? payload.tools : [],
    llmUsed: payload.llmUsed === true,
    degraded: payload.degraded === true
  }
}

function mapAgentStation(raw) {
  const mapped = mapStation(raw)
  if (!mapped) return null
  return {
    ...mapped,
    // 助手的结果带可用设备数，比列表多一项；缺失时退回空闲数，与列表行为一致。
    operationalCount: Number.isFinite(raw.operationalChargerCount)
      ? raw.operationalChargerCount
      : mapped.idleCount,
    // 契约给出价格拆分，卡片可显示构成而不只是一个总价。
    electricityPriceCentPerKwh: raw.electricityPriceCentPerKwh,
    servicePriceCentPerKwh: raw.servicePriceCentPerKwh
  }
}
