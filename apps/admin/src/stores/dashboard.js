import { defineStore } from 'pinia'
import {
  emptyChargerStatusStats,
  fetchChargerStatusStats,
  fetchRevenueStats,
  parseChargerStatusStats,
  parseRevenueStats
} from '../api/stats'
import { fetchUsers } from '../api/user'
import {
  endOfLocalDay,
  nowSeconds,
  secondsOfRecentDays,
  startOfLocalDay,
  toInteger,
  toNumber
} from '../utils/format'

/** 空营收汇总，保证页面结构在加载中与失败时不塌陷。 */
const emptyTotals = () => ({ amountCent: 0, energyMwh: 0, orderCount: 0 })

function totalsOf(stats) {
  return {
    amountCent: stats.totalAmountCent,
    energyMwh: stats.totalEnergyMwh,
    orderCount: stats.totalOrderCount
  }
}

/**
 * 运营总览数据（接口文档 §8.3、§8.4 与 §6.4）。
 * 今日/本月与趋势是三次独立的营收查询：服务端限制单次时间范围最大 90 天，
 * 拆开查询还能让 KPI 与趋势互不阻塞（任一失败时其余照常展示）。
 */
export const useDashboardStore = defineStore('adminDashboard', {
  state: () => ({
    /** 趋势区间天数（7/30）与桶粒度：day 用于趋势，hour 用于单日明细。 */
    rangeDays: 7,
    stationId: null,
    revenue: { items: [], totalAmountCent: 0, totalEnergyMwh: 0, totalOrderCount: 0 },
    today: emptyTotals(),
    month: emptyTotals(),
    chargerStatus: emptyChargerStatusStats(),
    userTotal: 0,
    selectedBucketStart: null,
    dayDetail: null,
    dayDetailLoading: false,
    dayDetailError: '',
    loading: false,
    error: '',
    notice: ''
  }),

  getters: {
    hasRevenue: state => state.revenue.items.length > 0,
    isEmpty: state =>
      !state.loading && !state.error && state.revenue.items.length === 0 && state.chargerStatus.totalCount === 0,
    /**
     * 健康度是小数（契约刻意保留两位：98.97 而不是 98，好让"少了一台设备"看得见）。
     * 这里必须用 toNumber：toInteger 对非整数返回 null，再 ?? 0 会把 98.97 变成 0，
     * 于是页面上"可运营 99004 / 总设备 100035"旁边写着 0.0%。
     */
    healthPercent: state => toNumber(state.chargerStatus.healthPercent) ?? 0,
    operationalCount: state => state.chargerStatus.operationalCount,
    totalChargers: state => state.chargerStatus.totalCount,
    /** 趋势图点位：时间为空或金额非法的记录直接跳过，不产生 NaN。 */
    points: state =>
      state.revenue.items
        .map(item => ({
          bucketStart: item.bucketStart,
          amount: item.amountCent / 100,
          energy: item.energyMwh / 1000000,
          orderCount: item.orderCount
        }))
        .filter(point => Number.isFinite(point.amount) && Number.isFinite(point.energy)),
    selectedPoint: state =>
      state.revenue.items.find(item => item.bucketStart === state.selectedBucketStart) || null,
    /** 单日明细：按小时桶聚合，用于“选中某一天”后的下钻表格。 */
    hourlyDetail: state => (state.dayDetail ? state.dayDetail.items : [])
  },

  actions: {
    async loadAll() {
      this.loading = true
      this.error = ''
      const failures = []
      const tasks = [
        this.loadRevenue().catch(error => failures.push(error?.userMessage || '营收统计加载失败')),
        this.loadToday().catch(error => failures.push(error?.userMessage || '今日营收加载失败')),
        this.loadMonth().catch(error => failures.push(error?.userMessage || '本月营收加载失败')),
        this.loadChargerStatus().catch(error => failures.push(error?.userMessage || '设备状态统计加载失败')),
        this.loadUserTotal().catch(() => failures.push('注册用户数加载失败'))
      ]
      await Promise.all(tasks)
      // 今日、本月和趋势都可能命中同一个未实现的统计能力；按文案去重，
      // 避免总览页把同一条提示重复渲染多次。
      this.error = [...new Set(failures)].join('；')
      this.loading = false
      return this.error === ''
    },

    /** 趋势区间：默认最近 30 天（服务端上限 90 天）。 */
    async loadRevenue() {
      const toAt = endOfLocalDay(nowSeconds())
      const fromAt = secondsOfRecentDays(this.rangeDays)
      const data = await fetchRevenueStats({ fromAt, toAt, stationId: this.stationId, bucket: 'day' })
      this.revenue = parseRevenueStats(data)
      if (!this.selectedBucketStart && this.revenue.items.length > 0) {
        this.selectedBucketStart = this.revenue.items[this.revenue.items.length - 1].bucketStart
      }
      return this.revenue
    },

    async loadToday() {
      const fromAt = startOfLocalDay(nowSeconds())
      const toAt = endOfLocalDay(nowSeconds())
      const data = await fetchRevenueStats({ fromAt, toAt, stationId: this.stationId, bucket: 'hour' })
      this.today = totalsOf(parseRevenueStats(data))
      return this.today
    },

    async loadMonth() {
      const start = startOfLocalDay(nowSeconds())
      const fromAt = start === null ? null : start - (new Date().getDate() - 1) * 86400
      const toAt = endOfLocalDay(nowSeconds())
      const data = await fetchRevenueStats({ fromAt, toAt, stationId: this.stationId, bucket: 'day' })
      this.month = totalsOf(parseRevenueStats(data))
      return this.month
    },

    async loadChargerStatus() {
      const data = await fetchChargerStatusStats({ stationId: this.stationId })
      this.chargerStatus = parseChargerStatusStats(data)
      return this.chargerStatus
    },

    /** 注册用户总数：复用用户列表的分页 total，不额外新增接口。 */
    async loadUserTotal() {
      const data = await fetchUsers({ page: 1, pageSize: 1 })
      this.userTotal = toInteger(data.total) ?? 0
      return this.userTotal
    },

    async setRangeDays(days) {
      this.rangeDays = toInteger(days) === 30 ? 30 : 7
      this.selectedBucketStart = null
      this.dayDetail = null
      try {
        await this.loadRevenue()
        this.error = ''
      } catch (error) {
        this.error = error?.userMessage || '营收统计加载失败'
      }
    },

    async setStationFilter(stationId) {
      this.stationId = toInteger(stationId) || null
      this.selectedBucketStart = null
      this.dayDetail = null
      return this.loadAll()
    },

    /** 选中某一天并拉取当天的小时明细。 */
    async selectDay(bucketStart) {
      const day = toInteger(bucketStart)
      if (day === null) return null
      this.selectedBucketStart = day
      this.dayDetailLoading = true
      this.dayDetailError = ''
      try {
        const data = await fetchRevenueStats({
          fromAt: startOfLocalDay(day),
          toAt: endOfLocalDay(day),
          stationId: this.stationId,
          bucket: 'hour'
        })
        this.dayDetail = parseRevenueStats(data)
        return this.dayDetail
      } catch (error) {
        this.dayDetail = null
        this.dayDetailError = error?.userMessage || '单日明细加载失败'
        return null
      } finally {
        this.dayDetailLoading = false
      }
    },

    reset() {
      this.revenue = { items: [], totalAmountCent: 0, totalEnergyMwh: 0, totalOrderCount: 0 }
      this.today = emptyTotals()
      this.month = emptyTotals()
      this.chargerStatus = emptyChargerStatusStats()
      this.userTotal = 0
      this.selectedBucketStart = null
      this.dayDetail = null
      this.dayDetailError = ''
      this.error = ''
      this.notice = ''
    }
  }
})
