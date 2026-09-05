import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { DashboardApiClient, DashboardError, dashboardApi, parseSummary } from '../src/api/client'
import { useDashboardStore } from '../src/stores/dashboardStore'
import { kwh, escapeHtml } from '../src/format'
import { summary, envelope } from './fixture'

beforeEach(() => {
  setActivePinia(createPinia())
  vi.stubGlobal('window', globalThis)
  vi.stubGlobal('localStorage', { removeItem: vi.fn() })
  vi.stubGlobal('sessionStorage', { removeItem: vi.fn() })
})
afterEach(() => {
  useDashboardStore().clearSession()
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
  vi.useRealTimers()
})
function authenticated() {
  const store = useDashboardStore()
  store.token = 'test-token'
  store.expiresAt = Date.now() + 8 * 3600_000
  store.lastActivity = Date.now()
  return store
}
describe('dashboard contract and safety', () => {
  it('accepts genuine zeros and converts integer mWh only for display', () => {
    const dto = summary()
    dto.revenue30d = [{ bucketAt: 1788235200, revenueCent: 123, orderCount: 1, energyMwh: 1_234_567 }]
    expect(parseSummary(dto)).toEqual(dto)
    expect(kwh(dto.revenue30d[0].energyMwh)).toBe(1.234567)
    expect(parseSummary(summary()).totalChargeCount).toBe(0)
  })
  it('rejects old energyKwh DTOs and numeric HTML injection', () => {
    const dto: any = summary()
    dto.stationRanking = [{ stationId: 1, stationName: '站点', orderCount: 1, energyKwh: 2, revenueCent: 3 }]
    expect(() => parseSummary(dto)).toThrow()
    dto.stationRanking[0].energyMwh = '<img src=x onerror=alert(1)>'
    expect(() => parseSummary(dto)).toThrow()
    expect(escapeHtml('<img src=x onerror="alert(1)">&')).toBe('&lt;img src=x onerror=&quot;alert(1)&quot;&gt;&amp;')
  })
  it.each([401, 403, 500])('never requests public or unimplemented snapshot routes after HTTP %s', async status => {
    const fetchMock = vi.fn().mockResolvedValue(new Response('{}', { status }))
    vi.stubGlobal('fetch', fetchMock)
    await expect(new DashboardApiClient().fetchSummary('token', new AbortController().signal)).rejects.toBeInstanceOf(DashboardError)
    expect(fetchMock).toHaveBeenCalledTimes(1)
    expect(fetchMock.mock.calls[0][0]).toBe('/api/v1/dashboard/summary')
  })
  it('uses the authenticated envelope and token', async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify(envelope(summary()))))
    vi.stubGlobal('fetch', fetchMock)
    await expect(new DashboardApiClient().fetchSummary('token', new AbortController().signal)).resolves.toEqual({ data: summary(), isFallback: false })
    expect(fetchMock.mock.calls[0][1].headers.Authorization).toBe('Bearer token')
  })
})
describe('session and refresh lifecycle', () => {
  it.each([401, 403])('clears data and stops polling on %s', async status => {
    vi.useFakeTimers()
    const store = authenticated()
    store.summary = summary()
    vi.spyOn(dashboardApi, 'fetchSummary').mockRejectedValue(new DashboardError(status, '会话失效'))
    store.startAutoRefresh()
    await vi.advanceTimersByTimeAsync(0)
    expect(store.token).toBeNull()
    expect(store.summary).toBeNull()
    expect(store.timerId).toBeNull()
    await vi.advanceTimersByTimeAsync(60_000)
    expect(dashboardApi.fetchSummary).toHaveBeenCalledTimes(1)
  })
  it('retains only a previous successful snapshot on outage and recovers', async () => {
    const store = authenticated()
    const mock = vi.spyOn(dashboardApi, 'fetchSummary')
    mock.mockRejectedValueOnce(new TypeError('network'))
    await store.loadData()
    expect(store.summary).toBeNull()
    expect(store.isFallback).toBe(false)
    mock.mockResolvedValueOnce({ data: summary(), isFallback: false })
    await store.loadData()
    mock.mockRejectedValueOnce(new TypeError('network'))
    await store.loadData()
    expect(store.summary).not.toBeNull()
    expect(store.isFallback).toBe(true)
    mock.mockResolvedValueOnce({ data: { ...summary(), dataVersion: 2 }, isFallback: false })
    await store.loadData()
    expect(store.isFallback).toBe(false)
    expect(store.error).toBeNull()
    expect(store.summary?.dataVersion).toBe(2)
  })
  it('does not restore data when an outstanding response arrives after logout', async () => {
    const store = authenticated()
    let resolve!: (value: { data: ReturnType<typeof summary>; isFallback: boolean }) => void
    vi.spyOn(dashboardApi, 'fetchSummary').mockImplementation(() => new Promise(r => { resolve = r }))
    const pending = store.loadData()
    store.clearSession()
    resolve({ data: summary(), isFallback: false })
    await pending
    expect(store.summary).toBeNull()
    expect(store.token).toBeNull()
  })
  it.each(['idle', 'absolute'])('expires %s sessions before allowing refresh', async kind => {
    const store = authenticated()
    vi.spyOn(dashboardApi, 'logout').mockResolvedValue({})
    const mock = vi.spyOn(dashboardApi, 'fetchSummary')
    if (kind === 'idle') store.lastActivity = Date.now() - 30 * 60_000
    else store.expiresAt = Date.now()
    await store.loadData()
    expect(store.token).toBeNull()
    expect(mock).not.toHaveBeenCalled()
  })
  it('does not extend idle time merely by polling', async () => {
    const store = authenticated()
    const activity = store.lastActivity
    vi.spyOn(dashboardApi, 'fetchSummary').mockResolvedValue({ data: summary(), isFallback: false })
    await store.loadData()
    expect(store.lastActivity).toBe(activity)
  })
})
