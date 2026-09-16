<script setup>
import { computed, onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import AppSkeleton from '@/components/AppSkeleton.vue'
import StationMap from '@/components/StationMap.vue'
import { createOrder } from '@/api/charging'
import { randomId } from '@/api/http'
import { PRESET_LOCATIONS } from '@/services/geolocation'
import { formatYuan } from '@/services/coordinate'
import { staggerStyle } from '@/composables/useReveal'
import { useAuthStore } from '@/stores/auth'
import { useStationStore } from '@/stores/station'

/**
 * 站点详情：设备列表、参考单价、评论墙与进入充电。
 * Go 契约按具体设备下单：开始充电 = 从当前桩型中选一台空闲设备创建订单；
 * 路线规划依赖 A-07 导航服务，当前显式提示未开放。
 */
const route = useRoute()
const router = useRouter()
const station = useStationStore()
const auth = useAuthStore()

const stationId = computed(() => route.params.stationId)
const busy = ref(false)
const actionError = ref('')

const idleChargers = computed(() => station.chargers.filter(charger => charger.status === 0))

const stationLocation = computed(() => {
  const detail = station.detail
  if (!detail || !Number.isFinite(detail.latitudeE6)) return null
  return { latitudeE6: detail.latitudeE6, longitudeE6: detail.longitudeE6, coordinateType: 'gcj02' }
})

const priceRows = computed(() => {
  const detail = station.detail
  if (!detail || !Number.isFinite(detail.minPriceCentPerKwh)) return []
  return [
    { label: '充电单价', value: `${formatYuan(detail.minPriceCentPerKwh)} 元/kWh 起` },
    { label: '空闲设备', value: `${detail.idleChargerCount ?? '—'} / ${detail.chargerCount ?? '—'} 台` }
  ]
})

onMounted(async () => {
  await station.loadStation(stationId.value)
  await Promise.all([station.loadChargers(stationId.value), station.loadReviews(stationId.value)])
})

async function switchChargerType(chargerType) {
  await station.loadChargers(stationId.value, { chargerType })
}

async function startCharging() {
  if (!auth.isLoggedIn) {
    void router.push({ name: 'profile', query: { redirect: `/stations/${stationId.value}` } })
    return
  }
  actionError.value = ''
  const charger = idleChargers.value[0]
  if (!charger) {
    actionError.value = '当前桩型暂无空闲设备，请稍后再试或选择其他桩型'
    return
  }
  busy.value = true
  try {
    // 创建订单即锁定设备与价格快照；旧契约的排队/报价确认阶段在 Go 模型中不存在。
    await createOrder({ chargerId: charger.id }, randomId())
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
        <h3>参考单价（结算以订单价格快照为准）</h3>
        <ul v-if="priceRows.length" class="metric-list">
          <li v-for="row in priceRows" :key="row.label"><span>{{ row.label }}</span><strong>{{ row.value }}</strong></li>
        </ul>
        <p v-else class="muted" data-testid="station-quote-empty">暂无价格信息</p>
      </div>

      <div v-reveal="{ delay: 180 }" class="panel" data-testid="station-route">
        <h3>路线规划</h3>
        <p class="muted" data-testid="station-route-unavailable">
          路线规划暂未开放：导航服务（A-07）尚未接入 Go 后端。站点位置与导航入口将在服务上线后提供。
        </p>
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

/* 路线规划入口（A-07 接入后恢复）；当前仅显示未开放说明。 */

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
