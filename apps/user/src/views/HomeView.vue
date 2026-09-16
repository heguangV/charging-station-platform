<script setup>
import { computed, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import AppSkeleton from '@/components/AppSkeleton.vue'
import StationCard from '@/components/StationCard.vue'
import StationMap from '@/components/StationMap.vue'
import { PRESET_LOCATIONS } from '@/services/geolocation'
import { useStationStore } from '@/stores/station'

/** 附近充电站：左侧列表 + 右侧地图（PC），移动端上下排列；地图失败不影响列表。 */
const router = useRouter()
const station = useStationStore()

const keywordInput = ref(station.keyword)
const chargerTypeFilter = ref(null)
const mobilePanel = ref('list')

const chargerTypeOptions = [
  { label: '全部', value: null },
  { label: '快充', value: 1 },
  { label: '慢充', value: 0 }
]

const locationNotice = computed(() => {
  const status = station.location.status
  if (status === 'failed' || status === 'unsupported') return station.location.error
  return ''
})

const mapLocation = computed(() =>
  station.hasLocation
    ? {
        latitudeE6: station.location.latitudeE6,
        longitudeE6: station.location.longitudeE6,
        coordinateType: station.location.coordinateType || 'wgs84'
      }
    : null
)

onMounted(async () => {
  // 首次进入先尝试定位；定位失败仍然按 keyword / 服务端默认位置查询站点。
  if (station.location.status === 'unknown') await station.locate()
  await station.search({ chargerType: chargerTypeFilter.value, page: 1 })
})

async function locateNow() {
  await station.locate()
  await station.search({ page: 1 })
}

async function usePreset(presetId) {
  station.usePresetLocation(presetId)
  await station.search({ page: 1 })
}

async function searchNow() {
  await station.search({ keyword: keywordInput.value, chargerType: chargerTypeFilter.value, page: 1 })
}

async function pickChargerType(value) {
  chargerTypeFilter.value = value
  await station.search({ chargerType: value, page: 1 })
}

function openStation(item) {
  void router.push(`/stations/${item.id}`)
}

function highlightFromMarker(stationId) {
  station.selectedStationId = stationId
  const matched = station.stations.find(item => String(item.id) === String(stationId))
  if (matched) void router.push(`/stations/${matched.id}`)
}
</script>

<template>
  <section class="view home-view" data-testid="home-view">
    <div v-reveal class="panel">
      <div class="panel__row">
        <button type="button" class="btn btn--primary" data-testid="home-locate" :disabled="station.location.status === 'loading'" @click="locateNow">
          {{ station.location.status === 'loading' ? '定位中…' : '定位我的位置' }}
        </button>
        <form class="inline-form" @submit.prevent="searchNow">
          <label class="sr-only" for="home-keyword">地址关键词</label>
          <input id="home-keyword" v-model="keywordInput" data-testid="home-keyword" type="search" placeholder="输入地址，例如：中关村大街 27 号" />
          <button type="submit" class="btn" data-testid="home-search">搜索</button>
        </form>
      </div>

      <div class="panel__row panel__row--wrap">
        <span class="muted">桩型：</span>
        <button
          v-for="option in chargerTypeOptions"
          :key="String(option.value)"
          type="button"
          class="chip"
          :class="{ 'is-active': chargerTypeFilter === option.value }"
          :data-testid="`home-charger-type-${option.value === null ? 'all' : option.value}`"
          @click="pickChargerType(option.value)"
        >
          {{ option.label }}
        </button>
      </div>

      <div v-if="!station.hasLocation" class="panel__row panel__row--wrap" data-testid="home-manual-fallback">
        <span class="muted">手动选择位置：</span>
        <button
          v-for="preset in PRESET_LOCATIONS"
          :key="preset.id"
          type="button"
          class="chip"
          :data-testid="`home-preset-${preset.id}`"
          @click="usePreset(preset.id)"
        >
          {{ preset.label }}
        </button>
      </div>

      <p v-if="locationNotice" class="alert alert--error" data-testid="home-location-error">{{ locationNotice }}</p>
      <p v-if="station.locationFallback" class="alert" data-testid="home-location-fallback">
        未获取到有效位置，当前展示的是服务端演示默认位置附近的站点。
      </p>
    </div>

    <div class="panel__row panel__row--switch">
      <button type="button" class="chip" :class="{ 'is-active': mobilePanel === 'list' }" data-testid="home-tab-list" @click="mobilePanel = 'list'">列表</button>
      <button type="button" class="chip" :class="{ 'is-active': mobilePanel === 'map' }" data-testid="home-tab-map" @click="mobilePanel = 'map'">地图</button>
    </div>

    <div v-reveal="{ delay: 80 }" class="split-layout" :data-panel="mobilePanel">
      <div class="split-layout__list" data-testid="home-station-list">
        <div v-if="station.loading" data-testid="home-loading" role="status">
          <span class="sr-only">正在加载附近站点…</span>
          <AppSkeleton variant="cards" :rows="3" />
        </div>

        <div v-else-if="station.error" class="alert alert--error" data-testid="home-error" role="alert">
          <p>{{ station.error }}</p>
          <button type="button" class="btn btn--primary" data-testid="home-retry" @click="searchNow">重试</button>
        </div>

        <p v-else-if="station.isEmpty" class="empty-state" data-testid="home-empty">
          附近没有找到符合条件的充电站，试试切换桩型或换个地址。
        </p>

        <TransitionGroup v-else name="list" tag="div" class="card-list" data-testid="home-stations">
          <StationCard
            v-for="item in station.stations"
            :key="item.id"
            class="stagger-item"
            :station="item"
            :selected="String(item.id) === String(station.selectedStationId)"
            :matched-charger-type="chargerTypeFilter"
            @select="openStation"
          />
        </TransitionGroup>
      </div>

      <div class="split-layout__map" data-testid="home-map-panel">
        <StationMap
          :stations="station.stations"
          :user-location="mapLocation"
          :highlight-id="station.selectedStationId"
          @marker-click="highlightFromMarker"
        />
      </div>
    </div>
  </section>
</template>
