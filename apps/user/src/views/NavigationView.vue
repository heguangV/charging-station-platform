<script setup>
import { computed, onMounted, ref } from 'vue'
import { useRoute } from 'vue-router'
import AppSkeleton from '@/components/AppSkeleton.vue'
import StationMap from '@/components/StationMap.vue'
import { fromE6, formatDistance, formatYuan, haversineMeter, wgs84ToGcj02 } from '@/services/coordinate'
import { NAVIGATION_MODES, buildRoutePlanUrl } from '@/services/navigation'
import { PRESET_LOCATIONS } from '@/services/geolocation'
import { useStationStore } from '@/stores/station'

/**
 * 导航页：从当前位置到选定站点的跳转式导航。
 *
 * 服务端路线规划（A-07）尚未接入 Go 后端，站内地图画线暂不可行；页面提供的是
 * 腾讯地图 URI API 的一键跳转（免 Key、不暴露服务端凭据），并诚实地说明这一
 * 降级。浏览器定位结果是 WGS-84，站点坐标按平台口径是 GCJ-02：距离与跳转
 * 参数都在本地换算成 GCJ-02 后计算，避免数百米的系统性偏差。
 */
const route = useRoute()
const station = useStationStore()

const stationId = computed(() => route.params.stationId)
const mode = ref('driving')
const openError = ref('')

const detail = computed(() => station.detail)

/** 站点坐标按平台口径即 GCJ-02（站点详情页同样按此渲染地图）。 */
const stationGcj02 = computed(() => {
  if (!detail.value || !Number.isFinite(detail.value.latitudeE6) || !Number.isFinite(detail.value.longitudeE6)) {
    return null
  }
  return { latitude: fromE6(detail.value.latitudeE6), longitude: fromE6(detail.value.longitudeE6) }
})

const userGcj02 = computed(() => {
  if (!station.hasLocation) return null
  const location = station.location
  const latitude = fromE6(location.latitudeE6)
  const longitude = fromE6(location.longitudeE6)
  if (location.coordinateType === 'gcj02') return { latitude, longitude }
  const converted = wgs84ToGcj02(latitude, longitude)
  return { latitude: converted.latitude, longitude: converted.longitude }
})

/** 距离只在同一坐标系（GCJ-02 对 GCJ-02）下计算。 */
const distanceMeter = computed(() => {
  if (!stationGcj02.value || !userGcj02.value) return null
  return haversineMeter(
    userGcj02.value.latitude,
    userGcj02.value.longitude,
    stationGcj02.value.latitude,
    stationGcj02.value.longitude
  )
})

const locationHint = computed(() => {
  if (!station.hasLocation) return ''
  if (station.location.source === 'preset') return `使用预设位置：${station.location.label || '手动选择'}（定位降级）`
  if (station.location.source === 'geolocation') {
    const accuracy = Number.isFinite(station.location.accuracyMeter)
      ? `，精度约 ${Math.round(station.location.accuracyMeter)} 米`
      : ''
    return `使用浏览器定位（WGS-84，已本地换算为 GCJ-02${accuracy}）`
  }
  return ''
})

/** 跳转链接；from 缺省时省略起点，由腾讯地图在设备端使用当前位置。 */
const routeUrl = computed(() => {
  if (!stationGcj02.value) return ''
  try {
    return buildRoutePlanUrl({
      from: userGcj02.value ? { ...userGcj02.value, name: '我的位置' } : null,
      to: { ...stationGcj02.value, name: detail.value.name || '充电站' },
      mode: mode.value
    })
  } catch {
    return ''
  }
})

const mapUserLocation = computed(() =>
  station.hasLocation
    ? {
        latitudeE6: station.location.latitudeE6,
        longitudeE6: station.location.longitudeE6,
        coordinateType: station.location.coordinateType
      }
    : null
)

onMounted(async () => {
  await station.loadStation(stationId.value)
})

async function locate() {
  openError.value = ''
  await station.locate()
}

function usePreset(presetId) {
  openError.value = ''
  station.usePresetLocation(presetId)
}

function openNavigation() {
  if (!routeUrl.value) {
    openError.value = '无法生成导航链接：站点坐标缺失或出行方式无效'
    return
  }
  openError.value = ''
  window.open(routeUrl.value, '_blank', 'noopener')
}
</script>

<template>
  <section class="view navigation-view" data-testid="navigation-view">
    <div v-if="station.detailLoading" data-testid="navigation-loading" role="status">
      <span class="sr-only">正在加载站点信息…</span>
      <AppSkeleton variant="cards" :rows="3" />
    </div>

    <template v-else-if="detail">
      <header v-reveal class="panel navigation-view__head" data-testid="navigation-header">
        <h2 data-testid="navigation-station-name">{{ detail.name }}</h2>
        <p class="muted" data-testid="navigation-station-address">{{ detail.address }}</p>
        <p class="muted">
          参考单价
          {{ Number.isFinite(detail.minPriceCentPerKwh) ? `${formatYuan(detail.minPriceCentPerKwh)} 元/kWh 起` : '暂无' }}
          <template v-if="distanceMeter !== null">
            · 距当前位置 <strong data-testid="navigation-distance">{{ formatDistance(distanceMeter) }}</strong>
          </template>
        </p>
      </header>

      <div v-reveal="{ delay: 60 }" class="panel">
        <h3>出行方式</h3>
        <div class="panel__row" data-testid="navigation-modes">
          <button
            v-for="item in NAVIGATION_MODES"
            :key="item.value"
            type="button"
            class="chip"
            :class="{ 'is-active': mode === item.value }"
            :data-testid="`navigation-mode-${item.value}`"
            @click="mode = item.value"
          >
            {{ item.label }}
          </button>
        </div>
      </div>

      <div v-reveal="{ delay: 100 }" class="panel">
        <h3>我的位置</h3>
        <p v-if="station.hasLocation" class="muted" data-testid="navigation-location-hint">{{ locationHint }}</p>
        <p v-else-if="station.location.status === 'failed' || station.location.status === 'unsupported'" class="alert alert--error" data-testid="navigation-location-error">
          {{ station.location.error }}
        </p>
        <p v-else class="muted" data-testid="navigation-location-empty">尚未定位：可获取当前位置，或选择预设位置继续。</p>

        <div class="panel__row panel__row--wrap">
          <button type="button" class="btn btn--primary" data-testid="navigation-locate" :disabled="station.location.status === 'loading'" @click="locate">
            {{ station.location.status === 'loading' ? '定位中…' : '获取当前位置' }}
          </button>
        </div>

        <div class="panel__row panel__row--wrap" data-testid="navigation-presets">
          <button
            v-for="preset in PRESET_LOCATIONS"
            :key="preset.id"
            type="button"
            class="chip"
            :class="{ 'is-active': station.hasLocation && station.location.source === 'preset' && station.location.latitudeE6 === preset.latitudeE6 }"
            :data-testid="`navigation-preset-${preset.id}`"
            @click="usePreset(preset.id)"
          >
            {{ preset.label }}
          </button>
        </div>
        <p class="muted">定位被拒绝或不可用时，可选择预设位置；跳转导航也可以不带起点，由腾讯地图在设备端使用当前位置。</p>
      </div>

      <div v-if="stationGcj02" class="panel" data-testid="navigation-map-panel">
        <h3>位置示意</h3>
        <StationMap
          :stations="[{ ...detail, coordinateType: 'gcj02' }]"
          :user-location="mapUserLocation"
          :highlight-id="detail.id"
          :zoom="14"
        />
      </div>

      <div v-reveal="{ delay: 160 }" class="panel">
        <h3>开始导航</h3>
        <p class="muted" data-testid="navigation-route-hint">
          <template v-if="!station.hasLocation">未定位：起点将由腾讯地图使用设备当前位置。</template>
          <template v-else>起点为当前选择的位置，终点为{{ detail.name }}。</template>
          移动端将唤起腾讯地图应用，电脑端打开网页版路线。
        </p>
        <p v-if="openError" class="alert alert--error" data-testid="navigation-open-error">{{ openError }}</p>
        <div class="panel__row">
          <button
            type="button"
            class="btn btn--primary"
            data-testid="navigation-open"
            :disabled="!routeUrl"
            @click="openNavigation"
          >
            在腾讯地图中{{ mode === 'driving' ? '驾车' : '步行' }}导航
          </button>
        </div>
      </div>

      <div v-if="!stationGcj02" class="panel" data-testid="navigation-no-coords">
        <h3>无法导航</h3>
        <p class="muted">该站点缺少坐标信息，暂时无法规划路线；请联系运营方核对站点档案。</p>
      </div>

      <div v-reveal="{ delay: 200 }" class="panel" data-testid="navigation-a07-note">
        <h3>关于路线绘制</h3>
        <p class="muted">
          站内地图画线导航依赖服务端路线规划（A-07）接入 Go 后端，开放前本页提供的是跳转式导航：
          坐标换算与距离计算在本地按 GCJ-02 完成，路线详情由腾讯地图给出。
        </p>
      </div>
    </template>

    <div v-else-if="station.error" class="alert alert--error" data-testid="navigation-error">
      <p>{{ station.error }}</p>
      <button type="button" class="btn btn--primary" @click="station.loadStation(stationId)">重试</button>
    </div>
  </section>
</template>

<style scoped>
.navigation-view__head {
  display: flex;
  flex-direction: column;
  gap: 2px;
  background:
    linear-gradient(140deg, rgba(13, 122, 111, 0.07), transparent 58%),
    var(--ncs-surface);
}

.navigation-view__head h2 {
  font-size: var(--ncs-fs-lg);
  letter-spacing: -0.02em;
}
</style>
