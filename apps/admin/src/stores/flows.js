import { defineStore } from 'pinia'
import { fetchFlows, forceReleaseFlow } from '../api/flow'
import { ERROR_CODES } from '../api/http'
import { toInteger } from '../utils/format'
import { FLOW_RELEASABLE_STATUS } from '../utils/domain'
import { useAuthStore } from './auth'

/**
 * 活动流程状态（接口文档 §8.1、§8.2）。
 * 强制释放只对状态 20/30 开放：其余状态由服务端保护（充电中必须走受控结算流程），
 * 客户端据此禁用按钮，但仍以服务端返回的 INVALID_STATE_TRANSITION 为准。
 */
export const useFlowsStore = defineStore('adminFlows', {
  state: () => ({
    items: [],
    total: 0,
    page: 1,
    pageSize: 20,
    filters: { status: null, stationId: '', chargerId: '', userId: '' },
    loading: false,
    saving: false,
    error: '',
    notice: '',
    conflict: '',
    selectedFlowNo: null
  }),

  getters: {
    isEmpty: state => !state.loading && !state.error && state.items.length === 0,
    selectedFlow: state => state.items.find(item => item.flowNo === state.selectedFlowNo) || null,
    releasableCount: state =>
      state.items.filter(item => FLOW_RELEASABLE_STATUS.includes(item.status)).length,
    pageCount: state => Math.max(1, Math.ceil(state.total / state.pageSize))
  },

  actions: {
    /** 数字过滤参数只接受正整数，其余按未提供处理。 */
    params() {
      const numeric = value => {
        const parsed = toInteger(typeof value === 'string' ? value.trim() || null : value)
        return parsed === null || parsed < 1 ? undefined : parsed
      }
      return {
        status: this.filters.status === null ? undefined : this.filters.status,
        stationId: numeric(this.filters.stationId),
        chargerId: numeric(this.filters.chargerId),
        userId: numeric(this.filters.userId),
        page: this.page,
        pageSize: this.pageSize
      }
    },

    async load() {
      this.loading = true
      this.error = ''
      try {
        const data = await fetchFlows(this.params())
        this.items = Array.isArray(data.items) ? data.items : []
        this.total = toInteger(data.total) ?? this.items.length
        this.page = toInteger(data.page) ?? this.page
        return true
      } catch (error) {
        this.items = []
        this.total = 0
        this.error = error?.userMessage || '活动流程加载失败，请稍后重试'
        return false
      } finally {
        this.loading = false
      }
    },

    setFilter(patch) {
      Object.assign(this.filters, patch)
      this.page = 1
      return this.load()
    },

    resetFilters() {
      this.filters = { status: null, stationId: '', chargerId: '', userId: '' }
      this.page = 1
      return this.load()
    },

    goToPage(page) {
      const target = toInteger(page)
      if (target === null || target < 1) return Promise.resolve(false)
      this.page = Math.min(target, this.pageCount)
      return this.load()
    },

    select(flowNo) {
      this.selectedFlowNo = flowNo
    },

    canRelease(flow) {
      return !!flow && FLOW_RELEASABLE_STATUS.includes(flow.status)
    },

    /** §8.2 强制释放：原因、目标设备状态与流程版本一起提交，成功后就地更新该行。 */
    async forceRelease(flow, { reason, nextChargerStatus }) {
      const auth = useAuthStore()
      this.saving = true
      this.error = ''
      this.notice = ''
      this.conflict = ''
      try {
        const data = await auth.runWithReauth(({ idempotencyKey }) =>
          forceReleaseFlow(
            flow.flowNo,
            { reason, nextChargerStatus, flowVersion: flow.version },
            { idempotencyKey }
          )
        )
        this.items = this.items.map(item =>
          item.flowNo === flow.flowNo
            ? { ...item, status: data.status, statusText: data.statusText, version: toInteger(data.version) ?? item.version }
            : item
        )
        this.notice = `流程 ${flow.flowNo} 已强制释放（${data.statusText || data.status}）`
        return true
      } catch (error) {
        if (error?.code === ERROR_CODES.VERSION_CONFLICT) {
          this.conflict = '该流程已被其他管理员处理，已刷新为最新状态'
          await this.load()
          return false
        }
        if (error?.code === ERROR_CODES.INVALID_STATE_TRANSITION) {
          this.conflict = '当前流程状态不允许强制释放（充电中请使用设备重启的受控结算流程）'
          await this.load()
          return false
        }
        this.error = error?.userMessage || '强制释放失败'
        return false
      } finally {
        this.saving = false
      }
    },

    clearMessages() {
      this.error = ''
      this.notice = ''
      this.conflict = ''
    }
  }
})
