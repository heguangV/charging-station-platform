<template>
  <transition name="slide-fade">
    <div v-if="store.isFallback || store.error" class="offline-banner">
      <div class="banner-content">
        <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" class="warning-icon">
          <path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"></path>
          <line x1="12" y1="9" x2="12" y2="13"></line>
          <line x1="12" y1="17" x2="12.01" y2="17"></line>
        </svg>
        <span class="banner-text">
          <b>容灾降级提示：</b>
          {{ store.summary ? '当前展示最近成功快照，数据可能过期；正在尝试恢复。' : '数据加载失败，尚无可用快照，请重试。' }}
        </span>
      </div>
      <button class="retry-btn" @click="store.loadData" :disabled="store.isLoading">
        {{ store.isLoading ? '重试中...' : '立即重试' }}
      </button>
    </div>
  </transition>
</template>

<script setup lang="ts">
import { useDashboardStore } from '../stores/dashboardStore'

const store = useDashboardStore()
</script>

<style scoped>
.offline-banner {
  background: linear-gradient(90deg, rgba(255, 61, 113, 0.9) 0%, rgba(255, 171, 0, 0.85) 100%);
  color: #ffffff;
  padding: 6px 24px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  font-size: 13px;
  box-shadow: 0 2px 10px rgba(255, 61, 113, 0.35);
  position: relative;
  z-index: 20;
}

.banner-content {
  display: flex;
  align-items: center;
  gap: 10px;
}

.warning-icon {
  flex-shrink: 0;
  animation: pulse 1.5s infinite;
}

.retry-btn {
  background: rgba(0, 0, 0, 0.25);
  border: 1px solid rgba(255, 255, 255, 0.6);
  color: #ffffff;
  padding: 3px 12px;
  border-radius: 4px;
  cursor: pointer;
  font-size: 12px;
  transition: background 0.2s ease;
}

.retry-btn:hover {
  background: rgba(0, 0, 0, 0.4);
}

.slide-fade-enter-active,
.slide-fade-leave-active {
  transition: all 0.3s ease;
}

.slide-fade-enter-from,
.slide-fade-leave-to {
  transform: translateY(-100%);
  opacity: 0;
}
</style>
