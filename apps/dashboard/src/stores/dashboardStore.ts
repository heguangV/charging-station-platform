import { defineStore } from 'pinia'
import { dashboardApi, DashboardError } from '../api/client'
import type { DashboardSummary } from '../types/dashboard'

let activeRequest: AbortController | null = null
let generation = 0
let sessionTimer: ReturnType<typeof setInterval> | null = null

export const useDashboardStore = defineStore('dashboard', {
  state: () => ({
    token: null as string | null,
    expiresAt: 0,
    lastActivity: 0,
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
      return date.toLocaleString('zh-CN', { hour12: false })
    }
  },

  actions: {
    async login(username: string, password: string) {
      if (this.isLoading || this.token) return
      const current = generation
      const request = new AbortController()
      activeRequest = request
      this.error = null
      this.isLoading = true
      try {
        const session = await dashboardApi.login(username, password, request.signal)
        if (current !== generation) return
        if (!session.accessToken || !Number.isSafeInteger(session.expiresAt) || session.expiresAt * 1000 <= Date.now()) throw new Error('登录响应无效')
        this.token = session.accessToken
        this.expiresAt = Math.min(session.expiresAt * 1000, Date.now() + 8 * 3600_000)
        this.lastActivity = Date.now()
        sessionTimer = setInterval(() => this.checkSession(), 1000)
        this.isLoading = false
        this.startAutoRefresh()
      } catch {
        if (current !== generation) return
        this.error = '登录失败，请检查账号密码或稍后重试'
        this.isLoading = false
      }
    },
    checkSession() {
      if (this.token && (Date.now() >= this.expiresAt || Date.now() - this.lastActivity >= 30 * 60_000)) {
        void this.logout('会话超时，请重新登录')
        return false
      }
      return !!this.token
    },
    recordActivity() {
      if (this.checkSession()) this.lastActivity = Date.now()
    },
    clearSession(message: string | null = null) {
      generation++
      activeRequest?.abort()
      activeRequest = null
      this.stopAutoRefresh()
      if (sessionTimer) clearInterval(sessionTimer)
      sessionTimer = null
      this.token = null
      this.summary = null
      this.isFallback = false
      this.lastUpdated = null
      this.isLoading = false
      this.error = message
      localStorage.removeItem('ncs_dashboard_token')
      sessionStorage.removeItem('ncs_dashboard_token')
    },
    async logout(message: unknown = null) {
      const token = this.token
      this.clearSession(typeof message === 'string' ? message : null)
      const current = generation
      if (token) {
        try { await dashboardApi.logout(token) } catch {
          if (current === generation && !this.token) this.error = '已退出本页；服务端退出请求失败，会话将按期限失效'
        }
      }
    },
    async loadData() {
      if (!this.checkSession() || this.isLoading) return
      this.isLoading = true
      this.error = null
      const current = generation
      const request = new AbortController()
      activeRequest = request
      try {
        const res = await dashboardApi.fetchSummary(this.token!, request.signal)
        if (current !== generation || !this.checkSession()) return
        this.summary = res.data
        this.isFallback = res.isFallback || res.data.stale
        this.lastUpdated = new Date()
      } catch (err: unknown) {
        if (current !== generation) return
        if (err instanceof DashboardError && err.authFailed) {
          this.clearSession(err.message)
          return
        }
        this.isFallback = !!this.summary
        this.error = '数据加载失败，请稍后重试'
      } finally {
        if (current === generation) {
          this.isLoading = false
          activeRequest = null
        }
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
