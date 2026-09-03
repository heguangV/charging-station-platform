import type { DashboardSummary, ApiResponse } from '../types/dashboard'

const API_TIMEOUT_MS = 6000

export class DashboardApiClient {
  private baseUrl: string

  constructor(baseUrl = '') {
    this.baseUrl = baseUrl
  }

  private getAuthToken(): string | null {
    return localStorage.getItem('ncs_dashboard_token') || sessionStorage.getItem('ncs_dashboard_token')
  }

  /**
   * 获取大屏核心汇总数据（双模降级保障机制）
   * 1. 优先调用后端受权接口 /api/v1/dashboard/summary
   * 2. 若接口超时或失败，自动回退拉取 /data/dashboard.json 本地最新快照
   */
  async fetchSummary(): Promise<{ data: DashboardSummary; isFallback: boolean }> {
    const controller = new AbortController()
    const timeoutId = setTimeout(() => controller.abort(), API_TIMEOUT_MS)

    const token = this.getAuthToken()
    const headers: Record<string, string> = {
      'Accept': 'application/json'
    }
    if (token) {
      headers['Authorization'] = `Bearer ${token}`
    }

    try {
      const resp = await fetch(`${this.baseUrl}/api/v1/dashboard/summary`, {
        method: 'GET',
        headers,
        signal: controller.signal
      })
      clearTimeout(timeoutId)

      if (!resp.ok) {
        throw new Error(`HTTP error ${resp.status}`)
      }

      const json = await resp.json() as ApiResponse<DashboardSummary> | DashboardSummary
      // 兼容 Result<T> 信封结构与纯数据结构
      if ('success' in json) {
        if (json.success && json.data) {
          return { data: json.data, isFallback: false }
        }
        throw new Error(json.message || 'Server returned failure')
      }

      return { data: json as DashboardSummary, isFallback: false }
    } catch (err) {
      clearTimeout(timeoutId)
      console.warn('[DashboardApiClient] Backend API unavailable, falling back to snapshot /data/dashboard.json:', err)
      return this.fetchOfflineSnapshot()
    }
  }

  /**
   * 读取离线降级快照文件
   */
  private async fetchOfflineSnapshot(): Promise<{ data: DashboardSummary; isFallback: boolean }> {
    try {
      const resp = await fetch('/data/dashboard.json?t=' + Date.now())
      if (!resp.ok) {
        throw new Error(`Fallback HTTP ${resp.status}`)
      }
      const data = await resp.json() as DashboardSummary
      data.stale = true
      return { data, isFallback: true }
    } catch (fallbackErr) {
      console.error('[DashboardApiClient] Failed to load offline snapshot:', fallbackErr)
      throw fallbackErr
    }
  }
}

export const dashboardApi = new DashboardApiClient()
