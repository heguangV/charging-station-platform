<script setup>
import { computed, onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import AppSkeleton from '@/components/AppSkeleton.vue'
import StationMap from '@/components/StationMap.vue'
import { requestFlow } from '@/api/charging'
import { randomId } from '@/api/http'
import { PRESET_LOCATIONS } from '@/services/geolocation'
import { formatDistance, formatDuration, formatYuan } from '@/services/coordinate'
import { staggerStyle } from '@/composables/useReveal'
import { useAuthStore } from '@/stores/auth'
import { useStationStore } from '@/stores/station'

/** 站点详情：设备列表、预估价格、评论墙、路线规划与进入充电流程。 */
const route = useRoute()
const router = useRouter()
const station = useStationStore()
const auth = useAuthStore()

const stationId = computed(() => route.params.stationId)
const mode = ref('driving')
const originKind = ref('current')
const originKeyword = ref('')
const busy = ref(false)
const actionError = ref('')

const modes = [
  { label: '驾车', value: 'driving' },
  { label: '步行', value: 'walking' },
  { label: '公交', value: 'transit' }
]

const routeResult = computed(() => station.route)
const navigateUrl = computed(() => station.route?.browserUrl || '')
const stationLocation = computed(() => {
  const detail = station.detail
  if (!detail || !Number.isFinite(detail.latitudeE6)) return null
  return { latitudeE6: detail.latitudeE6, longitudeE6: detail.longitudeE6, coordinateType: 'gcj02' }
})

const priceRows = computed(() => {
  const quote = station.quote
  if (!quote) return []
  return [
    { label: '电费', value: `${formatYuan(quote.electricityPriceCentPerKwh)} 元/kWh` },
    { label: '基础服务费', value: `${formatYuan(quote.baseServicePriceCentPerKwh)} 元/kWh` },
    { label: '最终服务费', value: `${formatYuan(quote.finalServicePriceCentPerKwh)} 元/kWh` },
    { label: '合计', value: `${formatYuan(quote.totalPriceCentPerKwh)} 元/kWh` }
  ]
})

onMounted(async () => {
  await station.loadStation(stationId.value)
  await Promise.all([
    station.loadChargers(stationId.value),
    station.loadQuote(stationId.value, station.chargerTypeTab),
    station.loadReviews(stationId.value)
  ])
})

async function switchChargerType(chargerType) {
  await Promise.all([station.loadChargers(stationId.value, { chargerType }), station.loadQuote(stationId.value, chargerType)])
}

async function planRoute() {
  actionError.value = ''
  if (originKind.value === 'keyword') {
    station.setKeyword(originKeyword.value)
    station.clearLocation()
  } else if (originKind.value !== 'current') {
    station.usePresetLocation(originKind.value)
  }
  await station.loadRoute(stationId.value, { mode: mode.value })
}

async function startCharging() {
  if (!auth.isLoggedIn) {
    void router.push({ name: 'profile', query: { redirect: `/charging?stationId=${stationId.value}` } })
    return
  }
  busy.value = true
  actionError.value = ''
  try {
    await requestFlow({ stationId: Number(stationId.value), chargerType: station.chargerTypeTab }, randomId())
    void router.push({ name: 'charging' })
  } catch (error) {
    actionError.value = error?.userMessage || '发起充电失败，请稍后重试'
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <section class="view station-view" data-testid="station-view">
    <div v-if="station.detailLoading" data-testid="station-loading" role="status">
      <span class="sr-only">正在加载站点详情…</span>
      <AppSkeleton variant="cards" :rows="3" />
    </div>

    <template v-else-if="station.detail">
      <header v-reveal class="panel station-view__head" data-testid="station-header">
        <h2 data-testid="station-name">{{ station.detail.name }}</h2>
        <p class="muted" data-testid="station-address">{{ station.detail.address }}</p>
        <p class="muted">
          营业时间：{{ station.detail.openingHours || '全天' }} · 可用桩
          {{ station.detail.operationalCount ?? '—' }} / {{ station.detail.totalCount ?? '—' }}
        </p>
      </header>

      <div v-reveal="{ delay: 60 }" class="panel">
        <div class="panel__row panel__row--wrap">
          <button type="button" class="chip" :class="{ 'is-active': station.chargerTypeTab === 1 }" data-testid="station-tab-fast" @click="switchChargerType(1)">快充</button>
          <button type="button" class="chip" :class="{ 'is-active': station.chargerTypeTab === 0 }" data-testid="station-tab-slow" @click="switchChargerType(0)">慢充</button>
          <button type="button" class="btn btn--primary" data-testid="station-start-charging" :disabled="busy" @click="startCharging">
            {{ busy ? '提交中…' : '开始充电' }}
          </button>
        </div>
        <p v-if="actionError" class="alert alert--error" data-testid="station-action-error">{{ actionError }}</p>

        <table class="data-table" data-testid="station-chargers">
          <thead>
            <tr><th>设备编号</th><th>类型</th><th>功率</th><th>接口</th><th>状态</th></tr>
          </thead>
          <tbody>
            <tr
              v-for="(charger, chargerIndex) in station.chargers"
              :key="charger.id"
              class="stagger-item"
              :style="staggerStyle(chargerIndex)"
            >
              <td>{{ charger.code }}</td>
              <td>{{ charger.chargerType === 1 ? '快充' : '慢充' }}</td>
              <td>{{ ((charger.powerWatt || 0) / 1000).toFixed(1) }} kW</td>
              <td>{{ charger.connectorStandard || '—' }}</td>
              <td>{{ charger.statusText || charger.status }}</td>
            </tr>
            <tr v-if="!station.chargers.length">
              <td colspan="5" class="muted" data-testid="station-chargers-empty">该桩型暂无设备</td>
            </tr>
          </tbody>
        </table>
      </div>

      <div v-reveal="{ delay: 120 }" class="panel" data-testid="station-quote">
        <h3>预估价格（仅供展示，结算以报价快照为准）</h3>
        <ul v-if="priceRows.length" class="metric-list">
          <li v-for="row in priceRows" :key="row.label"><span>{{ row.label }}</span><strong>{{ row.value }}</strong></li>
        </ul>
        <p v-else class="muted" data-testid="station-quote-empty">暂无报价</p>
      </div>

      <div v-reveal="{ delay: 180 }" class="panel" data-testid="station-route">
        <h3>路线规划</h3>
        <div class="panel__row panel__row--wrap">
          <button v-for="item in modes" :key="item.value" type="button" class="chip" :class="{ 'is-active': mode === item.value }" :data-testid="`station-mode-${item.value}`" @click="mode = item.value">
            {{ item.label }}
          </button>
        </div>
        <div class="panel__row panel__row--wrap">
          <label class="field">
            <span>起点</span>
            <select v-model="originKind" data-testid="station-origin-select">
              <option value="current">我的当前位置</option>
              <option value="keyword">输入地址</option>
              <option v-for="preset in PRESET_LOCATIONS" :key="preset.id" :value="preset.id">{{ preset.label }}</option>
            </select>
          </label>
          <input v-if="originKind === 'keyword'" v-model="originKeyword" data-testid="station-origin-keyword" type="text" placeholder="起点地址，例如：北京南站" />
          <button type="button" class="btn btn--primary" data-testid="station-plan-route" :disabled="station.routeLoading" @click="planRoute">
            {{ station.routeLoading ? '规划中…' : '规划路线' }}
          </button>
        </div>

        <p v-if="station.routeError" class="alert alert--error" data-testid="station-route-error">{{ station.routeError }}</p>

        <div v-if="routeResult" class="route-summary" data-testid="station-route-result">
          <p>距离 {{ formatDistance(routeResult.distanceMeter) }} · 预计 {{ formatDuration(routeResult.durationSecond) }}</p>
          <p v-if="routeResult.routeFallback" class="muted" data-testid="station-route-fallback">腾讯地图暂不可用，当前为本地降级距离。</p>
          <ol v-if="routeResult.steps?.length">
            <li v-for="(step, index) in routeResult.steps" :key="index">{{ step.instruction }}（{{ formatDistance(step.distanceMeter) }}）</li>
          </ol>
          <a v-if="navigateUrl" class="btn btn--primary" :href="navigateUrl" target="_blank" rel="noopener noreferrer" data-testid="station-navigate">导航</a>
        </div>
      </div>

      <div v-if="stationLocation" class="panel">
        <h3>站点位置</h3>
        <StationMap :stations="[{ ...station.detail, ...stationLocation }]" :user-location="station.hasLocation ? { latitudeE6: station.location.latitudeE6, longitudeE6: station.location.longitudeE6, coordinateType: station.location.coordinateType } : null" />
      </div>

      <div v-reveal="{ delay: 240 }" class="panel" data-testid="station-reviews">
        <h3>场站评价</h3>
        <ul v-if="station.reviews.length" class="review-list">
          <li v-for="(review, index) in station.reviews" :key="index">
            <strong>{{ review.author }}</strong>
            <span class="muted">{{ '★'.repeat(review.rating) }}</span>
            <p>{{ review.content }}</p>
          </li>
        </ul>
        <p v-else class="muted" data-testid="station-reviews-empty">暂无评价</p>
      </div>
    </template>

    <div v-else-if="station.error" class="alert alert--error" data-testid="station-error">
      <p>{{ station.error }}</p>
      <button type="button" class="btn btn--primary" @click="station.loadStation(stationId)">重试</button>
    </div>
  </section>
</template>

<style scoped>
.station-view__head {
  display: flex;
  flex-direction: column;
  gap: 2px;
  background:
    linear-gradient(140deg, rgba(13, 122, 111, 0.07), transparent 58%),
    var(--ncs-surface);
}

.station-view__head h2 {
  font-size: var(--ncs-fs-lg);
  letter-spacing: -0.02em;
}

/* 路线摘要：把距离/时长作为视觉主体，其余信息降级为辅助行 */
.route-summary {
  display: flex;
  flex-direction: column;
  gap: var(--ncs-s-2);
  padding: var(--ncs-s-4);
  border: 1px solid var(--ncs-line);
  border-radius: var(--ncs-r-md);
  background: linear-gradient(180deg, var(--ncs-brand-softer), transparent 60%);
  box-shadow: var(--ncs-shadow-1);
  animation: ncs-rise-in var(--ncs-dur-3) var(--ncs-ease-out) both;
}

.route-summary > p {
  margin: 0;
  font-size: var(--ncs-fs-md);
  font-weight: 650;
  color: var(--ncs-brand-strong);
  font-variant-numeric: tabular-nums;
}

.route-summary > ol {
  margin: 0;
  padding-left: 18px;
  font-size: var(--ncs-fs-sm);
  color: var(--ncs-text-2);
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.review-list {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: var(--ncs-s-3);
}

.review-list li {
  padding: var(--ncs-s-3);
  border: 1px solid var(--ncs-line);
  border-radius: var(--ncs-r-sm);
  background: var(--ncs-surface-2);
  transition: border-color var(--ncs-dur-2) var(--ncs-ease-out);
}

.review-list li:hover {
  border-color: var(--ncs-line-strong);
}

.review-list p {
  margin: 4px 0 0;
  font-size: var(--ncs-fs-sm);
}
</style>
