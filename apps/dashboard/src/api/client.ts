import type { DashboardSummary, DashboardSession, ApiResponse } from '../types/dashboard'

export class DashboardError extends Error {
  constructor(public readonly status: number, message: string) { super(message) }
  get authFailed() { return this.status === 401 || this.status === 403 }
}

// Reject incompatible DTOs before chart arithmetic/HTML formatters consume them.
export function parseSummary(value: unknown): DashboardSummary {
  const object = (v: unknown): Record<string, unknown> => {
    if (!v || typeof v !== 'object' || Array.isArray(v)) throw new DashboardError(422, '数据格式不兼容')
    return v as Record<string, unknown>
  }
  const numbers = (v: Record<string, unknown>, keys: string[]) => {
    for (const key of keys) if (!Number.isSafeInteger(v[key]) || Number(v[key]) < 0) throw new DashboardError(422, '数据格式不兼容')
  }
  const data = object(value)
  numbers(data, ['schemaVersion', 'dataVersion', 'generatedAt', 'totalRevenueCent', 'totalChargeCount', 'registeredUserCount', 'stationCount'])
  if (data.schemaVersion !== 1 || typeof data.stale !== 'boolean') throw new DashboardError(422, '数据版本不兼容')
  numbers(object(data.chargerStatus), ['idle', 'inUse', 'fault', 'restarting', 'disabled'])
  numbers(object(data.chargerTypeShare), ['fast', 'slow'])
  for (const [key, fields] of Object.entries({ revenue30d: ['bucketAt', 'revenueCent', 'orderCount', 'energyMwh'], stationRanking: ['stationId', 'orderCount', 'energyMwh', 'revenueCent'], hourlyHeatmap: ['weekday', 'hour', 'energyMwh', 'orderCount'], prediction24h: ['stationId', 'horizonHour', 'generatedAt', 'targetAt', 'predictedEnergyMwh', 'predictedFreeCount'] })) {
    if (!Array.isArray(data[key])) throw new DashboardError(422, '数据格式不兼容')
    for (const item of data[key]) {
      const row = object(item)
      numbers(row, fields)
      if (key === 'stationRanking' && typeof row.stationName !== 'string') throw new DashboardError(422, '数据格式不兼容')
      if (key === 'hourlyHeatmap' && (Number(row.hour) > 23 || Number(row.weekday) < 1 || Number(row.weekday) > 7)) throw new DashboardError(422, '时段格式不兼容')
      if (key === 'prediction24h' && (typeof row.isPeak !== 'boolean' || typeof row.stale !== 'boolean' || typeof row.modelVersionNo !== 'string')) throw new DashboardError(422, '预测格式不兼容')
    }
  }
  return data as unknown as DashboardSummary
}

export class DashboardApiClient {
  constructor(private baseUrl = '') {}
  async request<T>(path: string, token: string | null, signal: AbortSignal, body?: unknown): Promise<T> {
    const timeout = AbortSignal.timeout(6000)
    const resp = await fetch(`${this.baseUrl}/api/v1/dashboard${path}`, {
      method: body === undefined ? 'GET' : 'POST',
      headers: { Accept: 'application/json', ...(token ? { Authorization: `Bearer ${token}` } : {}), ...(body === undefined ? {} : { 'Content-Type': 'application/json' }) },
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: AbortSignal.any([signal, timeout]), cache: 'no-store'
    })
    if (!resp.ok) throw new DashboardError(resp.status, resp.status === 401 || resp.status === 403 ? '会话失效或无访问权限，请重新登录' : '服务暂时不可用')
    const json = await resp.json() as ApiResponse<T>
    if (json.success !== true || json.code !== 0 || !json.data) throw new DashboardError(422, '服务返回数据不可用')
    return json.data
  }
  login(username: string, password: string, signal: AbortSignal) {
    return this.request<DashboardSession>('/auth/login', null, signal, { username, password, deviceId: 'dashboard-browser' })
  }
  logout(token: string) { return this.request('/auth/logout', token, new AbortController().signal, {}) }
  async fetchSummary(token: string, signal: AbortSignal): Promise<{ data: DashboardSummary; isFallback: boolean }> {
    // The current backend exposes authenticated /summary only. It can return stale
    // snapshots. Do not invent a fallback route or fetch a public JSON file.
    return { data: parseSummary(await this.request('/summary', token, signal)), isFallback: false }
  }
}
export const dashboardApi = new DashboardApiClient()
