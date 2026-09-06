// REST DTOs use integer cents, integer milliwatt-hours and UTC Unix seconds.
export interface ChargerStatusCounts { idle: number; inUse: number; fault: number; restarting: number; disabled: number }
export interface DailyRevenueItem { bucketAt: number; revenueCent: number; orderCount: number; energyMwh: number }
export interface StationRankingItem { stationId: number; stationName: string; orderCount: number; energyMwh: number; revenueCent: number }
export interface HourlyHeatmapItem { weekday: number; hour: number; energyMwh: number; orderCount: number }
export interface Prediction24hItem { stationId: number; horizonHour: number; modelVersionNo: string; generatedAt: number; targetAt: number; predictedEnergyMwh: number; predictedFreeCount: number; isPeak: boolean; stale: boolean }
export interface DashboardSummary {
  schemaVersion: number
  dataVersion: number
  generatedAt: number
  stale: boolean
  totalRevenueCent: number
  totalChargeCount: number
  registeredUserCount: number
  stationCount: number
  chargerStatus: ChargerStatusCounts
  chargerTypeShare: { fast: number; slow: number }
  revenue30d: DailyRevenueItem[]
  stationRanking: StationRankingItem[]
  hourlyHeatmap: HourlyHeatmapItem[]
  prediction24h: Prediction24hItem[]
}
export interface DashboardSession { accessToken: string; expiresAt: number; sessionId: number }
export interface ApiResponse<T> { success: boolean; code: number; userMessage?: string; data: T }
