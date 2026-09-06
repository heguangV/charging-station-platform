import type { DashboardSummary } from '../src/types/dashboard'
export function summary(): DashboardSummary {
  return {
    schemaVersion: 1, dataVersion: 1, generatedAt: 1788235200, stale: false,
    totalRevenueCent: 0, totalChargeCount: 0, registeredUserCount: 0, stationCount: 0,
    chargerStatus: { idle: 0, inUse: 0, fault: 0, restarting: 0, disabled: 0 },
    chargerTypeShare: { fast: 0, slow: 0 },
    revenue30d: [], stationRanking: [], hourlyHeatmap: [], prediction24h: []
  }
}
export const envelope = (data: unknown) => ({ success: true, code: 0, data })
