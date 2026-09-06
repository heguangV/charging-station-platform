<template>
  <div class="metrics-container">
    <div class="metric-card">
      <div class="metric-icon-box cyan">
        <svg viewBox="0 0 24 24" width="24" height="24" fill="none" stroke="currentColor" stroke-width="2">
          <polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"></polygon>
        </svg>
      </div>
      <div class="metric-info">
        <div class="metric-label">近30日充电次数</div>
        <div class="metric-value">
          {{ formatNumber(store.summary?.totalChargeCount) }}
          <span class="metric-unit">次</span>
        </div>
        <div class="metric-sub">
          <span>覆盖电站</span>
          <span class="highlight">{{ store.summary?.stationCount ?? '--' }} 座</span>
        </div>
      </div>
    </div>

    <div class="metric-card">
      <div class="metric-icon-box gold">
        <svg viewBox="0 0 24 24" width="24" height="24" fill="none" stroke="currentColor" stroke-width="2">
          <circle cx="12" cy="12" r="9"></circle>
          <path d="M14.8 9A2 2 0 0 0 13 8h-2a2 2 0 0 0 0 4h2a2 2 0 0 1 0 4h-2a2 2 0 0 1-1.8-1"></path>
          <path d="M12 6v2m0 8v2"></path>
        </svg>
      </div>
      <div class="metric-info">
        <div class="metric-label">近30日总营收</div>
        <div class="metric-value text-gold">
          {{ store.summary ? formatCurrency(store.totalRevenueYuan) : '--' }}
          <span class="metric-unit">元</span>
        </div>
        <div class="metric-sub">
          <span>平均单价</span>
          <span class="highlight">--</span>
        </div>
      </div>
    </div>

    <div class="metric-card">
      <div class="metric-icon-box green">
        <svg viewBox="0 0 24 24" width="24" height="24" fill="none" stroke="currentColor" stroke-width="2">
          <rect x="2" y="3" width="20" height="14" rx="2" ry="2"></rect>
          <line x1="8" y1="21" x2="16" y2="21"></line>
          <line x1="12" y1="17" x2="12" y2="21"></line>
        </svg>
      </div>
      <div class="metric-info">
        <div class="metric-label">在线电桩数</div>
        <div class="metric-value text-green">
          {{ store.summary ? store.onlineChargers : '--' }}
          <span class="metric-unit">/ {{ store.summary ? store.totalChargers : '--' }} 台</span>
        </div>
        <div class="metric-sub">
          <span>设备在线率</span>
          <span class="highlight">{{ onlineRate }}</span>
        </div>
      </div>
    </div>

    <div class="metric-card">
      <div class="metric-icon-box purple">
        <svg viewBox="0 0 24 24" width="24" height="24" fill="none" stroke="currentColor" stroke-width="2">
          <path d="M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2"></path>
          <circle cx="9" cy="7" r="4"></circle>
          <path d="M23 21v-2a4 4 0 0 0-3-3.87"></path>
          <path d="M16 3.13a4 4 0 0 1 0 7.75"></path>
        </svg>
      </div>
      <div class="metric-info">
        <div class="metric-label">注册车主用户</div>
        <div class="metric-value text-purple">
          {{ formatNumber(store.summary?.registeredUserCount) }}
          <span class="metric-unit">人</span>
        </div>
        <div class="metric-sub">
          <span>账户活跃度</span>
          <span class="highlight">--</span>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useDashboardStore } from '../stores/dashboardStore'

const store = useDashboardStore()

const onlineRate = computed(() => {
  if (!store.totalChargers) return '--'
  return ((store.onlineChargers / store.totalChargers) * 100).toFixed(1) + '%'
})

const formatNumber = (num: number | undefined) => {
  return num?.toLocaleString() ?? '--'
}

const formatCurrency = (amount: number) => {
  return amount.toLocaleString(undefined, {
    minimumFractionDigits: 2,
    maximumFractionDigits: 2
  })
}
</script>

<style scoped>
.metrics-container {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 16px;
  width: 100%;
}

.metric-card {
  background: linear-gradient(135deg, rgba(20, 36, 68, 0.75) 0%, rgba(12, 22, 42, 0.75) 100%);
  border: 1px solid rgba(0, 210, 255, 0.25);
  border-radius: 8px;
  padding: 16px;
  display: flex;
  align-items: center;
  gap: 14px;
  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.3), inset 0 0 12px rgba(0, 210, 255, 0.05);
  transition: transform 0.2s ease, border-color 0.2s ease;
}

.metric-card:hover {
  transform: translateY(-2px);
  border-color: rgba(0, 210, 255, 0.6);
}

.metric-icon-box {
  width: 52px;
  height: 52px;
  border-radius: 10px;
  display: flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
}

.metric-icon-box.cyan {
  background: rgba(0, 210, 255, 0.15);
  color: #00d2ff;
  border: 1px solid rgba(0, 210, 255, 0.3);
}

.metric-icon-box.gold {
  background: rgba(255, 171, 0, 0.15);
  color: #ffab00;
  border: 1px solid rgba(255, 171, 0, 0.3);
}

.metric-icon-box.green {
  background: rgba(0, 230, 118, 0.15);
  color: #00e676;
  border: 1px solid rgba(0, 230, 118, 0.3);
}

.metric-icon-box.purple {
  background: rgba(187, 134, 252, 0.15);
  color: #bb86fc;
  border: 1px solid rgba(187, 134, 252, 0.3);
}

.metric-info {
  flex: 1;
  overflow: hidden;
}

.metric-label {
  font-size: 13px;
  color: var(--text-secondary);
  margin-bottom: 4px;
}

.metric-value {
  font-size: 22px;
  font-weight: 700;
  color: #ffffff;
  font-family: "SF Pro Display", -apple-system, monospace;
  white-space: nowrap;
}

.metric-value.text-gold {
  color: #ffc107;
  text-shadow: 0 0 8px rgba(255, 193, 7, 0.35);
}

.metric-value.text-green {
  color: #00e676;
  text-shadow: 0 0 8px rgba(0, 230, 118, 0.35);
}

.metric-value.text-purple {
  color: #bb86fc;
  text-shadow: 0 0 8px rgba(187, 134, 252, 0.35);
}

.metric-unit {
  font-size: 12px;
  font-weight: normal;
  color: var(--text-secondary);
  margin-left: 4px;
}

.metric-sub {
  font-size: 11px;
  color: var(--text-muted);
  margin-top: 4px;
  display: flex;
  justify-content: space-between;
}

.metric-sub .highlight {
  color: var(--primary-color);
  font-weight: 500;
}
</style>
