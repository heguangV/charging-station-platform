<script setup>
import { computed, onBeforeUnmount, onMounted, ref, shallowRef, watch } from 'vue'
import { fromE6, wgs84ToGcj02 } from '@/services/coordinate'
import { MapLoadError, MapLoadErrorKind, loadTencentMap } from '@/services/tencentMap'

/**
 * 腾讯地图站点组件：直接使用网页内的腾讯地图 GL JS（TMap），不使用 WebView / iframe。
 * 三种状态必须可见：loading / ready / error（error 携带重试按钮与失败原因）。
 * 地图失败时站点列表仍然可用——组件只负责地图区域本身。
 */
const props = defineProps({
  stations: { type: Array, default: () => [] },
  userLocation: { type: Object, default: null },
  highlightId: { type: [String, Number], default: null },
  zoom: { type: Number, default: 12 },
  height: { type: String, default: '340px' }
})

const emit = defineEmits(['marker-click', 'ready', 'error'])

const DEFAULT_CENTER = { latitude: 39.9042, longitude: 116.4074 }
const MARKER_SRC = 'https://mapapi.qq.com/web/lbs/javascriptGL/demo/img/markerDefault.png'
const HIGHLIGHT_SRC = '/navigation-chevron.svg'

const state = ref('loading')
const errorKind = ref('')
const errorMessage = ref('')
const container = ref(null)
const mapInstance = shallowRef(null)
const markerLayer = shallowRef(null)
const markerHandler = shallowRef(null)
let disposed = false

const isMissingKey = computed(() => errorKind.value === MapLoadErrorKind.MISSING_KEY)

/** WGS-84 坐标（浏览器定位）需要转成 GCJ-02 才能与腾讯地图底图对齐。 */
function toLatLng(TMap, latitudeE6, longitudeE6, coordinateType) {
  let latitude = fromE6(latitudeE6)
  let longitude = fromE6(longitudeE6)
  if (coordinateType === 'wgs84') {
    const converted = wgs84ToGcj02(latitude, longitude)
    latitude = converted.latitude
    longitude = converted.longitude
  }
  return new TMap.LatLng(latitude, longitude)
}

function stationGeometries(TMap) {
  return props.stations
    .filter(station => Number.isFinite(station.latitudeE6) && Number.isFinite(station.longitudeE6))
    .map(station => ({
      id: String(station.id),
      styleId: String(station.id) === String(props.highlightId) ? 'highlight' : 'station',
      position: toLatLng(TMap, station.latitudeE6, station.longitudeE6, station.coordinateType),
      properties: { title: station.name || '' }
    }))
}

function markerStyles(TMap) {
  const base = { width: 26, height: 34, anchor: { x: 13, y: 34 }, src: MARKER_SRC }
  return {
    station: new TMap.MarkerStyle(base),
    highlight: new TMap.MarkerStyle({ ...base, width: 34, height: 44, anchor: { x: 17, y: 44 }, src: HIGHLIGHT_SRC })
  }
}

function centerLatLng(TMap) {
  const location = props.userLocation
  if (location && Number.isInteger(location.latitudeE6) && Number.isInteger(location.longitudeE6)) {
    return toLatLng(TMap, location.latitudeE6, location.longitudeE6, location.coordinateType || 'wgs84')
  }
  const first = props.stations.find(station => Number.isFinite(station.latitudeE6) && Number.isFinite(station.longitudeE6))
  if (first) return toLatLng(TMap, first.latitudeE6, first.longitudeE6, first.coordinateType)
  return new TMap.LatLng(DEFAULT_CENTER.latitude, DEFAULT_CENTER.longitude)
}

function destroyMap() {
  try {
    if (markerLayer.value && markerHandler.value) markerLayer.value.off?.('click', markerHandler.value)
    markerLayer.value?.setMap?.(null)
  } catch {
    /* 地图已销毁时忽略。 */
  }
  try {
    mapInstance.value?.destroy?.()
  } catch {
    /* 地图已销毁时忽略。 */
  }
  markerLayer.value = null
  mapInstance.value = null
  markerHandler.value = null
}

async function initMap() {
  state.value = 'loading'
  errorKind.value = ''
  errorMessage.value = ''
  try {
    const TMap = await loadTencentMap()
    if (disposed) return
    if (!container.value) throw new MapLoadError(MapLoadErrorKind.NO_API, '地图容器尚未就绪，请点击重试。')

    mapInstance.value = new TMap.Map(container.value, { center: centerLatLng(TMap), zoom: props.zoom })
    markerHandler.value = event => {
      const id = event?.geometry?.id
      if (id !== undefined && id !== null) emit('marker-click', String(id))
    }
    markerLayer.value = new TMap.MultiMarker({
      id: 'ncs-station-markers',
      map: mapInstance.value,
      styles: markerStyles(TMap),
      geometries: stationGeometries(TMap)
    })
    markerLayer.value.on?.('click', markerHandler.value)
    state.value = 'ready'
    emit('ready')
  } catch (error) {
    if (disposed) return
    errorKind.value = error instanceof MapLoadError ? error.kind : MapLoadErrorKind.NO_API
    errorMessage.value = error?.userMessage || '地图加载失败，请稍后重试。'
    state.value = 'error'
    emit('error', errorMessage.value)
  }
}

function refreshGeometries() {
  if (state.value !== 'ready' || !markerLayer.value || typeof window === 'undefined' || !window.TMap) return
  markerLayer.value.setGeometries?.(stationGeometries(window.TMap))
}

function centerMap() {
  if (state.value !== 'ready' || !mapInstance.value || typeof window === 'undefined' || !window.TMap) return
  mapInstance.value.setCenter?.(centerLatLng(window.TMap))
}

function retry() {
  destroyMap()
  void initMap()
}

onMounted(() => {
  void initMap()
})

onBeforeUnmount(() => {
  disposed = true
  destroyMap()
})

watch(() => [props.stations, props.highlightId], refreshGeometries, { deep: false })
watch(() => props.userLocation, centerMap, { deep: false })

defineExpose({ retry, state })
</script>

<template>
  <section class="station-map" data-testid="station-map" :data-state="state" :style="{ minHeight: height }">
    <div ref="container" class="station-map__canvas" data-testid="station-map-canvas" :style="{ height }"></div>

    <p
      v-if="state === 'loading'"
      class="station-map__overlay station-map__overlay--loading overlay-rise"
      data-testid="station-map-loading"
      role="status"
    >
      <span class="spinner" aria-hidden="true"></span>
      <span class="waiting-pulse">地图加载中，正在连接腾讯地图…</span>
    </p>

    <div
      v-else-if="state === 'error'"
      class="station-map__overlay station-map__overlay--error overlay-pop"
      data-testid="station-map-error"
      role="alert"
    >
      <p data-testid="station-map-error-message">{{ errorMessage }}</p>
      <p v-if="isMissingKey" class="station-map__hint" data-testid="station-map-missing-key">
        尚未配置腾讯地图 Key：请在仓库根目录 .env 中设置 TENCENT_MAP_JS_KEY 后重启前端服务。
      </p>
      <button type="button" class="btn btn--primary" data-testid="station-map-retry" @click="retry">重试加载地图</button>
      <p class="station-map__hint">地图不可用时，右侧站点列表与导航按钮仍可正常使用。</p>
    </div>

    <p v-if="state === 'ready'" class="station-map__badge overlay-pop" data-testid="station-map-ready">
      地图已加载
    </p>
  </section>
</template>

<style scoped>
.station-map {
  position: relative;
  border-radius: var(--ncs-r-lg);
  overflow: hidden;
  background: linear-gradient(160deg, #e8eef6, #dfe7f1);
  border: 1px solid var(--ncs-line);
  box-shadow: var(--ncs-shadow-1);
}

/* 地图就绪后淡入，避免瓦片逐块出现时的割裂感 */
.station-map__canvas {
  width: 100%;
  animation: ncs-fade-in var(--ncs-dur-3) var(--ncs-ease-out) both;
}

.station-map__overlay {
  position: absolute;
  left: var(--ncs-s-3);
  right: var(--ncs-s-3);
  top: var(--ncs-s-3);
  margin: 0;
  padding: 12px 14px;
  border-radius: var(--ncs-r-sm);
  background: rgba(255, 255, 255, 0.9);
  backdrop-filter: blur(10px);
  -webkit-backdrop-filter: blur(10px);
  font-size: var(--ncs-fs-sm);
  color: var(--ncs-muted);
  box-shadow: var(--ncs-shadow-2);
  border: 1px solid rgba(255, 255, 255, 0.7);
}

.station-map__overlay--loading {
  display: flex;
  align-items: center;
  gap: 10px;
  color: var(--ncs-text-2);
}

.station-map__overlay--error {
  display: flex;
  flex-direction: column;
  gap: var(--ncs-s-2);
  align-items: flex-start;
  background: var(--ncs-danger-soft);
  border-color: rgba(200, 56, 47, 0.2);
  color: #8d2a23;
}

.station-map__overlay--error::before {
  content: '';
  position: absolute;
  left: 0;
  top: 0;
  bottom: 0;
  width: 3px;
  background: var(--ncs-danger);
}

.station-map__overlay--error > p {
  margin: 0;
}

.station-map__hint {
  margin: 0;
  font-size: var(--ncs-fs-xs);
  opacity: 0.86;
}

.station-map__badge {
  position: absolute;
  right: var(--ncs-s-3);
  bottom: var(--ncs-s-3);
  margin: 0;
  padding: 5px 12px;
  border-radius: var(--ncs-r-pill);
  background: rgba(13, 122, 111, 0.92);
  backdrop-filter: blur(6px);
  -webkit-backdrop-filter: blur(6px);
  color: #fff;
  font-size: var(--ncs-fs-xs);
  font-weight: 600;
  letter-spacing: 0.02em;
  box-shadow: 0 4px 14px rgba(13, 122, 111, 0.28);
}
</style>
