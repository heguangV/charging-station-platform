export interface ChargerStatusCounts {
  idle: number        // 空闲
  inUse: number       // 充电使用中
  fault: number       // 故障
  restarting: number  // 正在重启/恢复
  disabled: number    // 停用/维护
}

export interface DailyRevenueItem {
  date: string        // YYYY-MM-DD
  revenueCent: number // 当日营收（分）
  chargeCount: number // 当日充电次数
  energyKwh: number   // 当日充电量（度）
}

export interface StationRankingItem {
  stationId: number
  stationName: string
  chargeCount: number
  energyKwh: number
  revenueCent: number
}

export interface HourlyHeatmapItem {
  hour: number        // 0 ~ 23
  loadPercent: number // 负荷百分比 0 ~ 100
}

export interface Prediction24hItem {
  targetTime: number  // UTC 秒
  hourLabel: string   // 如 "14:00"
  predictedEnergy: number // 预测充电量（度）
  predictedFreeChargers: number // 预测空闲桩数
  isPeak: number      // 0=平时, 1=高峰
}

export interface ChargerTypeCounts {
  fast: number        // 快充桩数
  slow: number        // 慢充桩数
}

export interface DashboardSummary {
  schemaVersion: number
  dataVersion: number
  generatedAt: number // 生成时间戳 (UTC 秒)
  stale: boolean      // 是否过期/降级数据
  totalRevenueCent: number
  totalChargeCount?: number
  registeredUserCount: number
  stationCount: number
  chargerStatus: ChargerStatusCounts
  chargerTypeRatio?: ChargerTypeCounts
  revenue30d: DailyRevenueItem[]
  stationRanking: StationRankingItem[]
  hourlyHeatmap: HourlyHeatmapItem[]
  prediction24h: Prediction24hItem[]
}

export interface ApiResponse<T> {
  success: boolean
  code: number
  message: string
  userMessage?: string
  data: T
}
