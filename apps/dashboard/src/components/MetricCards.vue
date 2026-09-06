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
.metrics-container { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 12px; }
.metric-card { border: 1px solid #e3e8ed; border-radius: 16px; background: #fff; padding: 18px; display: flex; gap: 12px; align-items: flex-start; min-width: 0; }
.metric-card:nth-child(2) { background: #fff8ed; border-color: #f5e2c9; }
.metric-card:nth-child(3) { background: #edf7f1; border-color: #dcece1; }
.metric-icon-box { width: 36px; height: 36px; border-radius: 10px; display: grid; place-items: center; flex-shrink: 0; background: #eef3f7; color: #204760; }
.metric-icon-box.gold { background: #ffead0; color: #be6718; }
.metric-icon-box.green { background: #dcefe3; color: #17744f; }
.metric-icon-box.purple { background: #eef0f4; color: #506176; }
.metric-info { flex: 1; min-width: 0; }
.metric-label { font-size: 14px; color: #53616c; margin-bottom: 8px; }
.metric-value { font-size: clamp(25px, 1.9vw, 36px); line-height: 1.3; font-weight: 800; color: #183649; font-variant-numeric: tabular-nums; overflow-wrap: anywhere; }
.text-gold { color: #d36c17; }
.text-green { color: #17754f; }
.metric-unit { font-size: 12px; font-weight: 400; color: #53616c; }
.metric-sub { display: flex; flex-wrap: wrap; justify-content: space-between; gap: 4px; margin-top: 8px; font-size: 12px; color: #606c76; }
.highlight { color: #334b5b; }
@media (min-width: 1101px) and (max-width: 1500px) { .metric-card { padding: 12px; gap: 8px; } .metric-icon-box { display: none; } }
@media (max-width: 420px) { .metric-card { padding: 14px; } .metric-icon-box { display: none; } }

</style>
