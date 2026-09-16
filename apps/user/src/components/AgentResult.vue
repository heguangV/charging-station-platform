<script setup>
import { computed } from 'vue'
import { useRouter } from 'vue-router'
import PoiCard from './PoiCard.vue'
import StationCard from './StationCard.vue'
import { staggerStyle } from '@/composables/useReveal'
import { formatDistance, formatDuration } from '@/services/coordinate'

/**
 * AI 助手结构化结果渲染：站点卡片、POI 卡片、路线摘要与 actions 按钮。
 * 后端契约：stations[] / pois[] / route|null / actions[] / tools[] / llmUsed / degraded。
 */
const props = defineProps({
  result: { type: Object, required: true }
})

const emit = defineEmits(['action'])
const router = useRouter()

const stations = computed(() => (Array.isArray(props.result.stations) ? props.result.stations : []))
const pois = computed(() => (Array.isArray(props.result.pois) ? props.result.pois : []))
const route = computed(() => props.result.route || null)
const actions = computed(() => (Array.isArray(props.result.actions) ? props.result.actions : []))
const tools = computed(() => (Array.isArray(props.result.tools) ? props.result.tools : []))
const degraded = computed(() => props.result.degraded === true)
const isEmpty = computed(() => stations.value.length === 0 && pois.value.length === 0 && !route.value)

const navigateUrl = computed(() => {
  const action = actions.value.find(item => item.type === 'navigate' && item.url)
  return action?.url || route.value?.browserUrl || ''
})

function poiNavigateUrl(poi) {
  const action = actions.value.find(item => item.type === 'navigate' && item.url && String(item.targetId ?? '') === String(poi.id))
  return action?.url || ''
}

/** open_station 在应用内跳转站点详情；navigate 在新标签页打开腾讯地图 URL。 */
function runAction(action) {
  if (action.type === 'navigate' && action.url) window.open(action.url, '_blank', 'noopener,noreferrer')
  else if (action.type === 'open_station' && action.targetId !== undefined && action.targetId !== null) {
    void router.push(`/stations/${action.targetId}`)
  }
  emit('action', action)
}

function openStation(station) {
  void router.push(`/stations/${station.id}`)
}
</script>

<template>
  <section class="agent-result" data-testid="agent-result">
    <p v-if="degraded" class="agent-result__banner" data-testid="agent-degraded">
      AI 或地图服务暂时不可用，以下为确定性查询结果。
    </p>

    <div v-if="isEmpty" class="empty-state" data-testid="agent-empty-result">
      <p>没有找到匹配的充电站或周边地点，可以换一个说法或补充位置信息后重试。</p>
      <p class="muted" data-testid="agent-empty-stations">站点结果为空</p>
      <p class="muted" data-testid="agent-empty-pois">周边地点结果为空</p>
      <p class="muted" data-testid="agent-empty-route">未生成路线</p>
    </div>

    <template v-else>
      <div v-if="stations.length" class="agent-result__section">
        <h4 class="agent-result__heading">推荐充电站</h4>
        <div class="card-list" data-testid="agent-stations">
          <StationCard
            v-for="(station, index) in stations"
            :key="station.id"
            class="stagger-item"
            :style="staggerStyle(index)"
            :station="station"
            @select="openStation"
          />
        </div>
      </div>

      <div v-if="pois.length" class="agent-result__section">
        <h4 class="agent-result__heading">周边地点</h4>
        <div class="card-list" data-testid="agent-pois">
          <PoiCard
            v-for="(poi, index) in pois"
            :key="poi.id"
            class="stagger-item"
            :style="staggerStyle(index)"
            :poi="poi"
            :url="poiNavigateUrl(poi)"
          />
        </div>
      </div>

      <div v-if="route" class="route-card" data-testid="agent-route">
        <h4 class="agent-result__heading">路线摘要</h4>
        <p data-testid="agent-route-destination">目的地：{{ route.destinationName || '—' }}</p>
        <p class="route-card__metrics">
          <span data-testid="agent-route-distance">{{ formatDistance(route.distanceMeter) }}</span>
          <span data-testid="agent-route-duration">{{ formatDuration(route.durationSecond) }}</span>
          <span v-if="route.provider" class="muted">{{ route.provider }}</span>
        </p>
        <p v-if="route.fallback" class="muted" data-testid="agent-route-fallback">路线服务降级，仅提供直线距离参考。</p>
        <ol v-if="Array.isArray(route.steps) && route.steps.length" class="route-card__steps">
          <li v-for="(step, index) in route.steps" :key="index">
            {{ step.instruction }}（{{ formatDistance(step.distanceMeter) }}）
          </li>
        </ol>
        <a
          v-if="navigateUrl"
          class="btn btn--primary"
          data-testid="agent-route-navigate"
          :href="navigateUrl"
          target="_blank"
          rel="noopener noreferrer"
          >导航</a
        >
      </div>
    </template>

    <div v-if="actions.length" class="agent-result__actions" data-testid="agent-actions">
      <button
        v-for="(action, index) in actions"
        :key="`${action.type}-${index}`"
        type="button"
        class="btn btn--ghost"
        data-testid="agent-action"
        @click="runAction(action)"
      >
        {{ action.label || action.type }}
      </button>
    </div>

    <p v-if="tools.length" class="muted agent-result__tools" data-testid="agent-tools">
      已调用工具：{{ tools.join(' / ') }}
    </p>
  </section>
</template>

<style scoped>
.agent-result {
  display: flex;
  flex-direction: column;
  gap: var(--ncs-s-3);
}

/* 降级提示：与站点卡片同一套语义色，但用琥珀区分“结果可用、能力受限” */
.agent-result__banner {
  margin: 0;
  padding: 10px 14px;
  border-radius: var(--ncs-r-sm);
  background: var(--ncs-accent-soft);
  color: #7a4a06;
  font-size: var(--ncs-fs-sm);
  border-left: 3px solid var(--ncs-accent);
  animation: ncs-rise-in var(--ncs-dur-3) var(--ncs-ease-out) both;
}

.agent-result__section {
  display: flex;
  flex-direction: column;
  gap: var(--ncs-s-2);
}

.agent-result__heading {
  margin: 0;
  font-size: var(--ncs-fs-xs);
  font-weight: 700;
  letter-spacing: 0.09em;
  color: var(--ncs-muted);
  text-transform: uppercase;
}

.agent-result__actions {
  display: flex;
  flex-wrap: wrap;
  gap: var(--ncs-s-2);
}

/* 路线摘要：用左侧品牌条与内嵌指标条突出“距离 / 时长”两个关键数字 */
.route-card {
  display: flex;
  flex-direction: column;
  gap: var(--ncs-s-2);
  padding: var(--ncs-s-4);
  border: 1px solid var(--ncs-line);
  border-radius: var(--ncs-r-md);
  background: linear-gradient(180deg, var(--ncs-brand-softer), transparent 55%);
  box-shadow: var(--ncs-shadow-1);
  animation: ncs-rise-in var(--ncs-dur-3) var(--ncs-ease-out) both;
}

.route-card p {
  margin: 0;
}

.route-card__metrics {
  display: flex;
  flex-wrap: wrap;
  align-items: baseline;
  gap: var(--ncs-s-3);
  font-variant-numeric: tabular-nums;
}

.route-card__metrics > span:first-child {
  font-size: var(--ncs-fs-xl);
  font-weight: 700;
  letter-spacing: -0.02em;
  color: var(--ncs-brand-strong);
}

.route-card__metrics > span:nth-child(2) {
  font-size: var(--ncs-fs-md);
  font-weight: 650;
  color: var(--ncs-text-2);
}

.route-card__steps {
  margin: 0;
  padding-left: 18px;
  font-size: var(--ncs-fs-sm);
  color: var(--ncs-text-2);
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.route-card__steps li {
  transition: color var(--ncs-dur-2) var(--ncs-ease-out);
}

.route-card__steps li:hover {
  color: var(--ncs-brand-strong);
}

.agent-result :deep(.empty-state) {
  display: flex;
  flex-direction: column;
  gap: 4px;
}
</style>
