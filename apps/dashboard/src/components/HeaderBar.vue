<template>
  <header class="dashboard-header">
    <div class="header-left">
      <div class="status-badge" :class="{ 'is-fallback': store.isFallback }">
        <span class="pulse-dot"></span>
        <span class="status-text">{{ store.isFallback ? '离线快照模式' : '实时在线' }}</span>
      </div>
      <div class="meta-item">
        <span class="meta-label">数据截止:</span>
        <span class="meta-value">{{ store.formattedDataTime }}</span>
      </div>
      <div class="meta-item">
        <span class="meta-label">数据版本:</span>
        <span class="meta-value">v{{ store.summary?.dataVersion || '1.0' }}</span>
      </div>
    </div>

    <div class="header-center">
      <div class="title-glow"></div>
      <h1 class="header-title">NCS 充电桩综合运营监控大屏</h1>
      <div class="sub-title">NETWORK CHARGING SYSTEM INTELLIGENT OPERATION DASHBOARD</div>
    </div>

    <div class="header-right">
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
.dashboard-header {
  height: 84px;
  width: 100%;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 24px;
  background: linear-gradient(180deg, rgba(16, 32, 60, 0.85) 0%, rgba(10, 20, 38, 0.4) 100%);
  border-bottom: 2px solid rgba(0, 210, 255, 0.35);
  box-shadow: 0 4px 20px rgba(0, 180, 255, 0.15);
  position: relative;
  z-index: 10;
}

.header-left {
  display: flex;
  align-items: center;
  gap: 16px;
  flex: 1;
}

.status-badge {
  display: flex;
  align-items: center;
  gap: 8px;
  background: rgba(0, 230, 118, 0.12);
  border: 1px solid rgba(0, 230, 118, 0.35);
  padding: 4px 12px;
  border-radius: 16px;
  font-size: 13px;
  color: var(--success-color);
  font-weight: 500;
}

.status-badge.is-fallback {
  background: rgba(255, 171, 0, 0.12);
  border-color: rgba(255, 171, 0, 0.35);
  color: var(--warning-color);
}

.pulse-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: currentColor;
  box-shadow: 0 0 8px currentColor;
  animation: pulse 2s infinite;
}

@keyframes pulse {
  0% { transform: scale(0.95); opacity: 0.8; }
  50% { transform: scale(1.2); opacity: 1; }
  100% { transform: scale(0.95); opacity: 0.8; }
}

.meta-item {
  font-size: 13px;
  color: var(--text-secondary);
}

.meta-label {
  margin-right: 4px;
  color: var(--text-muted);
}

.meta-value {
  color: var(--primary-color);
  font-family: monospace;
}

.header-center {
  text-align: center;
  position: relative;
}

.header-title {
  font-size: 28px;
  font-weight: 800;
  letter-spacing: 4px;
  background: linear-gradient(180deg, #ffffff 20%, #a8d5ff 70%, #00d2ff 100%);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
  text-shadow: 0 2px 12px rgba(0, 210, 255, 0.5);
  margin-bottom: 2px;
}

.sub-title {
  font-size: 10px;
  letter-spacing: 2px;
  color: rgba(141, 164, 196, 0.7);
  font-family: monospace;
}

.header-right {
  display: flex;
  align-items: center;
  gap: 16px;
  flex: 1;
  justify-content: flex-end;
}

.clock-display {
  display: flex;
  flex-direction: column;
  align-items: flex-end;
  font-family: "SF Pro Display", monospace;
}

.clock-date {
  font-size: 12px;
  color: var(--text-secondary);
}

.clock-time {
  font-size: 18px;
  font-weight: 700;
  color: var(--accent-color);
  text-shadow: 0 0 10px rgba(0, 240, 255, 0.5);
}

.action-btn {
  background: rgba(26, 45, 78, 0.6);
  border: 1px solid rgba(0, 210, 255, 0.35);
  color: var(--text-primary);
  width: 34px;
  height: 34px;
  border-radius: 6px;
  display: flex;
  align-items: center;
  justify-content: center;
  cursor: pointer;
  transition: all 0.25s ease;
}

.action-btn:hover {
  background: rgba(0, 210, 255, 0.25);
  border-color: var(--accent-color);
  box-shadow: 0 0 10px rgba(0, 240, 255, 0.4);
}

.spin {
  animation: spin 1s linear infinite;
}

@keyframes spin {
  from { transform: rotate(0deg); }
  to { transform: rotate(360deg); }
}
</style>
