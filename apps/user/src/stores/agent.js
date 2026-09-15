import { defineStore } from 'pinia'
import { chatWithAgent } from '../api/agent'
import { randomId } from '../api/http'

/** 对话消息 id 仅用于列表渲染，不含任何个人信息。 */
function assistantMessage(content, extra = {}) {
  return { id: randomId(), role: 'assistant', content, createdAt: Date.now() / 1000, ...extra }
}

/**
 * AI 助手会话状态。结构化结果（stations / pois / route / actions / tools / degraded）
 * 原样保留，由组件负责渲染，不在 store 里做展示层换算。
 */
export const useAgentStore = defineStore('userAgent', {
  state: () => ({
    messages: [],
    lastResult: null,
    lastRequest: null,
    loading: false,
    error: ''
  }),

  getters: {
    /** 有对话内容或正在请求时才算活跃会话。 */
    hasConversation: state => state.messages.length > 0,
    isEmptyResult: state => {
      if (!state.lastResult) return false
      const { stations, pois, route } = state.lastResult
      return (!stations || stations.length === 0) && (!pois || pois.length === 0) && !route
    }
  },

  actions: {
    /**
     * 发送一条消息。参数与 POST /user/agent/chat 契约一致：
     * message 必填；location 可选但必须成对；coordinateType 默认 gcj02。
     */
    async send(message, { location, coordinateType, chargerType } = {}) {
      const text = typeof message === 'string' ? message.trim() : ''
      if (!text) return false

      const payload = { message: text }
      if (location && Number.isInteger(location.latitudeE6) && Number.isInteger(location.longitudeE6)) {
        payload.location = { latitudeE6: location.latitudeE6, longitudeE6: location.longitudeE6 }
        payload.coordinateType = coordinateType || location.coordinateType || 'gcj02'
      }
      if (chargerType === 0 || chargerType === 1) payload.chargerType = chargerType

      this.messages.push({ id: randomId(), role: 'user', content: text, createdAt: Date.now() / 1000 })
      this.lastRequest = payload
      this.loading = true
      this.error = ''
      try {
        const result = await chatWithAgent(payload)
        this.lastResult = result
        this.messages.push(assistantMessage(result?.reply || '已完成查询。', { result }))
        return true
      } catch (error) {
        this.error = error?.userMessage || 'AI 助手暂时不可用，请稍后重试'
        return false
      } finally {
        this.loading = false
      }
    },

    /** 失败后重试上一次请求，使用同一条消息，避免重复输入。 */
    async retry() {
      if (!this.lastRequest) return false
      return this.send(this.lastRequest.message, {
        location: this.lastRequest.location,
        coordinateType: this.lastRequest.coordinateType,
        chargerType: this.lastRequest.chargerType
      })
    },

    reset() {
      this.messages = []
      this.lastResult = null
      this.lastRequest = null
      this.error = ''
      this.loading = false
    }
  }
})
