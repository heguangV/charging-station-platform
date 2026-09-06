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
.offline-banner { background: #fff1bc; color: #755718; border-bottom: 1px solid #e8d78f; padding: 10px 24px; display: flex; align-items: center; justify-content: space-between; gap: 12px; font-size: 13px; line-height: 1.6; }
.banner-content { display: flex; align-items: center; gap: 10px; }
.warning-icon { flex-shrink: 0; }
.retry-btn { flex-shrink: 0; background: #fffaf0; color: #755718; border: 1px solid #d8bf71; border-radius: 8px; padding: 7px 12px; }

</style>
