<script setup>
import { computed } from 'vue'
import AgentChat from '@/components/AgentChat.vue'
import { useStationStore } from '@/stores/station'

/** AI 助手页：所有工具编排、LLM 与地图调用都在服务端完成，前端只渲染结构化结果。 */
const station = useStationStore()

const locationHint = computed(() => {
  if (!station.hasLocation) return '未定位，可在「附近」页获取定位或手动选择位置'
  const label = station.location.label ? `${station.location.label} ` : ''
  const latitude = (station.location.latitudeE6 / 1e6).toFixed(4)
  const longitude = (station.location.longitudeE6 / 1e6).toFixed(4)
  return `当前位置：${label}${latitude}, ${longitude}（WGS-84，由服务端转换为 GCJ-02）`
})
</script>

<template>
  <section class="view agent-view" data-testid="agent-view">
    <div v-reveal class="panel agent-view__intro">
      <h2>AI 助手</h2>
      <p class="muted">
        描述你的需求，例如「附近有快充并且旁边能吃饭的充电站」，助手会通过服务端聚合站点、周边地点与路线。
      </p>
      <p class="muted" data-testid="agent-location-hint">{{ locationHint }}</p>
    </div>

    <AgentChat />
  </section>
</template>
