<template>
  <header class="dashboard-header">
    <div class="header-left">
      <div class="status-badge" :class="{ 'is-fallback': store.isFallback }">
        <span class="pulse-dot"></span>
        <span class="status-text">{{ !store.summary ? (store.isLoading ? '加载中' : '数据不可用') : store.isFallback ? '离线快照模式' : '实时在线' }}</span>
      </div>
      <div class="meta-item">
        <span class="meta-label">数据截止:</span>
        <span class="meta-value">{{ store.formattedDataTime }}</span>
      </div>
      <div class="meta-item">
        <span class="meta-label">数据版本:</span>
        <span class="meta-value">v{{ store.summary?.dataVersion ?? '--' }}</span>
      </div>
    </div>

    <div class="header-center">
      <div class="title-glow"></div>
      <h1 class="header-title">NCS 充电桩综合运营监控大屏</h1>
      <div class="sub-title">NETWORK CHARGING SYSTEM INTELLIGENT OPERATION DASHBOARD</div>
    </div>

    <div class="header-right">
      <button class="action-btn" title="退出登录" @click="store.logout">退出</button>
      <div class="clock-display">
        <span class="clock-date">{{ currentDateStr }}</span>
        <span class="clock-time">{{ currentTimeStr }}</span>
      </div>
      <button class="action-btn" title="手动刷新" @click="handleManualRefresh" :disabled="store.isLoading">
        <svg viewBox="0 0 24 24" width="16" height="16" stroke="currentColor" fill="none" :class="{ 'spin': store.isLoading }">
          <path d="M23 4v6h-6M1 20v-6h6"></path>
          <path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"></path>
        </svg>
      </button>
      <button class="action-btn" title="全屏切换" @click="toggleFullscreen">
        <svg viewBox="0 0 24 24" width="16" height="16" stroke="currentColor" fill="none">
          <path v-if="!isFullscreen" d="M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3"></path>
          <path v-else d="M8 3v3a2 2 0 0 1-2 2H3m18 0h-3a2 2 0 0 1-2-2V3m0 18v-3a2 2 0 0 1 2-2h3M3 16h3a2 2 0 0 1 2 2v3"></path>
        </svg>
      </button>
    </div>
  </header>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'
import { useDashboardStore } from '../stores/dashboardStore'

const store = useDashboardStore()
const currentDateStr = ref('')
const currentTimeStr = ref('')
const isFullscreen = ref(false)
let clockTimer: number | null = null

const updateClock = () => {
  const now = new Date()
  const days = ['周日', '周一', '周二', '周三', '周四', '周五', '周六']
  const year = now.getFullYear()
  const month = String(now.getMonth() + 1).padStart(2, '0')
  const date = String(now.getDate()).padStart(2, '0')
  const day = days[now.getDay()]
  currentDateStr.value = `${year}-${month}-${date} ${day}`

  const hours = String(now.getHours()).padStart(2, '0')
  const minutes = String(now.getMinutes()).padStart(2, '0')
  const seconds = String(now.getSeconds()).padStart(2, '0')
  currentTimeStr.value = `${hours}:${minutes}:${seconds}`
}

const handleManualRefresh = () => {
  store.loadData()
}

const toggleFullscreen = () => {
  if (!document.fullscreenElement) {
    document.documentElement.requestFullscreen().catch((err) => {
      console.warn('进入全屏失败:', err)
    })
    isFullscreen.value = true
  } else {
    document.exitFullscreen().catch((err) => {
      console.warn('退出全屏失败:', err)
    })
    isFullscreen.value = false
  }
}

const onFullscreenChange = () => {
  isFullscreen.value = !!document.fullscreenElement
}

onMounted(() => {
  updateClock()
  clockTimer = window.setInterval(updateClock, 1000)
  document.addEventListener('fullscreenchange', onFullscreenChange)
})

onUnmounted(() => {
  if (clockTimer !== null) {
    clearInterval(clockTimer)
  }
  document.removeEventListener('fullscreenchange', onFullscreenChange)
})
</script>

<style scoped>
.dashboard-header { display: grid; grid-template-columns: 1fr auto; gap: 12px 24px; padding: 20px 24px 16px; background: #fff; border-bottom: 1px solid var(--panel-border); }
.header-center { grid-row: 1; grid-column: 1; }
.header-title { font-size: clamp(22px, 1.65vw, 32px); line-height: 1.3; font-weight: 800; letter-spacing: 1px; }
.sub-title { margin-top: 5px; color: var(--text-secondary); font-size: 10px; letter-spacing: 1.8px; }
.title-glow { display: none; }
.header-left { grid-row: 2; grid-column: 1 / -1; display: flex; align-items: center; gap: 18px; flex-wrap: wrap; font-size: 13px; }
.status-badge { display: flex; align-items: center; gap: 7px; padding: 4px 10px; background: #e6f4e9; color: #18734b; border-radius: 20px; font-weight: 600; }
.status-badge.is-fallback { background: #fff2ba; color: #805e00; }
.pulse-dot { width: 7px; height: 7px; border-radius: 50%; background: currentColor; }
.meta-label { color: var(--text-secondary); margin-right: 5px; }
.meta-value { font-variant-numeric: tabular-nums; }
.header-right { grid-row: 1; grid-column: 2; display: flex; align-items: center; gap: 10px; }
.clock-display { display: flex; flex-direction: column; align-items: flex-end; margin: 0 8px; font-variant-numeric: tabular-nums; }
.clock-date { color: var(--text-secondary); font-size: 11px; }
.clock-time { font-size: 23px; font-weight: 700; }
.action-btn { background: #f2f6ef; border: 1px solid #d5e1d6; color: #295d42; min-width: 38px; height: 38px; border-radius: 10px; display: grid; place-items: center; padding: 0 8px; }
.action-btn:hover { background: #e1f0df; }
.spin { animation: spin 1s linear infinite; }
@keyframes spin { to { transform: rotate(360deg); } }
@media (min-width: 1600px) { .dashboard-header { grid-template-columns: 1fr auto; } }
@media (max-width: 700px) { .dashboard-header { display: flex; flex-direction: column; padding: 18px 16px; } .header-right { justify-content: flex-start; } .header-title { font-size: 22px; } .sub-title { letter-spacing: 0; font-size: 9px; } }

 .header-title { font-weight: 800; letter-spacing: 0; color: #172b3a; }
.clock-time { color: #172b3a; }
.action-btn { background: #fff; border-color: #dde3e8; }
</style>
