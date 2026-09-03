<template>
  <div class="scale-wrapper" :style="wrapperStyle">
    <div class="dashboard-root">
      <OfflineAlert />
      <HeaderBar />

      <main class="dashboard-main">
        <!-- 左列 (30%) -->
        <section class="column column-left">
          <div class="card-wrapper flex-1">
            <ChargerStatusChart />
          </div>
          <div class="card-wrapper flex-1">
            <StationRankChart />
          </div>
        </section>

        <!-- 中列 (40%) -->
        <section class="column column-center">
          <div class="metrics-wrapper">
            <MetricCards />
          </div>
          <div class="card-wrapper flex-grow">
            <RevenueTrendChart />
          </div>
        </section>

        <!-- 右列 (30%) -->
        <section class="column column-right">
          <div class="card-wrapper flex-1">
            <HourlyHeatmapChart />
          </div>
          <div class="card-wrapper flex-1">
            <ChargerTypeChart />
          </div>
          <div class="card-wrapper flex-1">
            <LoadPredictionChart />
          </div>
        </section>
      </main>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { useDashboardStore } from './stores/dashboardStore'
import HeaderBar from './components/HeaderBar.vue'
import OfflineAlert from './components/OfflineAlert.vue'
import MetricCards from './components/MetricCards.vue'
import ChargerStatusChart from './components/ChargerStatusChart.vue'
import StationRankChart from './components/StationRankChart.vue'
import RevenueTrendChart from './components/RevenueTrendChart.vue'
import HourlyHeatmapChart from './components/HourlyHeatmapChart.vue'
import ChargerTypeChart from './components/ChargerTypeChart.vue'
import LoadPredictionChart from './components/LoadPredictionChart.vue'

const store = useDashboardStore()

// 1920 x 1080 基准大屏分辨率等比自适应缩放
const TARGET_WIDTH = 1920
const TARGET_HEIGHT = 1080
const scale = ref(1)

const updateScale = () => {
  const currentWidth = window.innerWidth
  const currentHeight = window.innerHeight
  const scaleX = currentWidth / TARGET_WIDTH
  const scaleY = currentHeight / TARGET_HEIGHT
  scale.value = Math.min(scaleX, scaleY)
}

const wrapperStyle = computed(() => ({
  width: `${TARGET_WIDTH}px`,
  height: `${TARGET_HEIGHT}px`,
  transform: `scale(${scale.value})`,
  transformOrigin: 'center center'
}))

onMounted(() => {
  updateScale()
  window.addEventListener('resize', updateScale)
  // 启动 30 秒轮询调度 (UC-W-02)
  store.startAutoRefresh(30000)
})

onUnmounted(() => {
  window.removeEventListener('resize', updateScale)
  store.stopAutoRefresh()
})
</script>

<style scoped>
.scale-wrapper {
  position: absolute;
  overflow: hidden;
  transition: transform 0.15s ease-out;
}

.dashboard-root {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  background: radial-gradient(circle at 50% 15%, #132442 0%, #090f1c 70%, #050812 100%);
  position: relative;
  overflow: hidden;
}

.dashboard-main {
  flex: 1;
  display: flex;
  gap: 16px;
  padding: 16px 20px 20px 20px;
  height: calc(1080px - 84px);
  overflow: hidden;
}

.column {
  display: flex;
  flex-direction: column;
  gap: 16px;
  height: 100%;
}

.column-left {
  flex: 3;
}

.column-center {
  flex: 4;
}

.column-right {
  flex: 3;
}

.metrics-wrapper {
  width: 100%;
}

.card-wrapper {
  width: 100%;
  position: relative;
}

.flex-1 {
  flex: 1;
  min-height: 0;
}

.flex-grow {
  flex: 1;
  min-height: 0;
}
</style>
