import { defineStore } from 'pinia'
import { dashboardApi } from '../api/client'
import type { DashboardSummary } from '../types/dashboard'

export const useDashboardStore = defineStore('dashboard', {
  state: () => ({
    summary: null as DashboardSummary | null,
    isFallback: false,
    isLoading: false,
    error: null as string | null,
    lastUpdated: null as Date | null,
    timerId: null as number | null
  }),

  getters: {
    totalRevenueYuan(state): number {
      if (!state.summary) return 0
      return Math.round((state.summary.totalRevenueCent / 100) * 100) / 100
    },

    totalChargers(state): number {
      if (!state.summary?.chargerStatus) return 0
      const { idle, inUse, fault, restarting, disabled } = state.summary.chargerStatus
      return (idle || 0) + (inUse || 0) + (fault || 0) + (restarting || 0) + (disabled || 0)
    },

    onlineChargers(state): number {
      if (!state.summary?.chargerStatus) return 0
      const { idle, inUse } = state.summary.chargerStatus
      return (idle || 0) + (inUse || 0)
    },

    formattedDataTime(state): string {
      if (!state.summary?.generatedAt) return '--'
      const date = new Date(state.summary.generatedAt * 1000)
      return date.toLocaleTimeString('zh-CN', { hour12: false })
    }
  },

  actions: {
    async loadData() {
      this.isLoading = true
      this.error = null
      try {
        const res = await dashboardApi.fetchSummary()
        this.summary = res.data
        this.isFallback = res.isFallback || res.data.stale
        this.lastUpdated = new Date()
      } catch (err: unknown) {
        this.error = err instanceof Error ? err.message : '加载大屏数据失败'
      } finally {
        this.isLoading = false
      }
    },

    startAutoRefresh(intervalMs = 30000) {
      this.stopAutoRefresh()
      this.loadData()
      this.timerId = window.setInterval(() => {
        this.loadData()
      }, intervalMs)
    },

    stopAutoRefresh() {
      if (this.timerId !== null) {
        window.clearInterval(this.timerId)
        this.timerId = null
      }
    }
  }
})
