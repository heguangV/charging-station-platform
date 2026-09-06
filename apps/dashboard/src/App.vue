<template>
  <div class="scale-wrapper" >
    <LoginPanel v-if="!store.token" />
    <div v-else class="dashboard-root">
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
import { onMounted, onUnmounted } from 'vue'
import { useDashboardStore } from './stores/dashboardStore'
import LoginPanel from './components/LoginPanel.vue'
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

onMounted(() => {
  // 启动 30 秒轮询调度 (UC-W-02)
  for (const event of ['pointerdown', 'keydown', 'pointermove']) window.addEventListener(event, store.recordActivity)
  document.addEventListener('visibilitychange', store.checkSession)
})

onUnmounted(() => {
  store.clearSession()
  for (const event of ['pointerdown', 'keydown', 'pointermove']) window.removeEventListener(event, store.recordActivity)
  document.removeEventListener('visibilitychange', store.checkSession)
})
</script>

<style scoped>
.scale-wrapper { width: 100%; min-height: 100vh; }
.dashboard-root { height: 100vh; min-height: 740px; display: flex; flex-direction: column; }
.dashboard-main { flex: 1; display: grid; grid-template-columns: minmax(0, 3fr) minmax(0, 4fr) minmax(0, 3fr); gap: 20px; padding: 20px 24px 24px; min-height: 0; }
.column { min-width: 0; min-height: 0; display: flex; flex-direction: column; gap: 20px; }
.card-wrapper { min-height: 0; width: 100%; }
.flex-1, .flex-grow { flex: 1; }
.column-right > :nth-child(2) { flex: .65; }
.column-right > :nth-child(3) { flex: 1.35; }
@media (max-width: 1500px) { .dashboard-main { gap: 12px; padding: 16px; } .column { gap: 12px; } }
@media (max-width: 1100px) { .dashboard-root { height: auto; } .dashboard-main { height: auto; grid-template-columns: 1fr 1fr; } .column-center { grid-column: 1 / -1; grid-row: 1; } .flex-1, .flex-grow { flex: auto; height: 320px; } }
@media (max-width: 640px) { .dashboard-main { grid-template-columns: minmax(0, 1fr); padding: 12px; } .column-center { grid-column: auto; } }

</style>
