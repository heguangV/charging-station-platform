import { defineStore } from 'pinia'
import { approveAppeal, fetchAppeals, rejectAppeal } from '../api/appeals'
import { toInteger } from '../utils/format'

/**
 * 管理端申诉队列（UC-U-09，A-04 第 8/9 步）。
 * 审核通过由 Go 契约保证幂等：重复审核是无操作成功，直接返回当前结果，
 * 因此前端不需要为审核维护幂等键，也不存在"重复审核重复退款"的风险。
 */
export const useAppealsStore = defineStore('adminAppeals', {
  state: () => ({
    items: [],
    total: 0,
    page: 1,
    pageSize: 20,
    filters: { status: null },
    loading: false,
    saving: false,
    error: '',
    notice: ''
  }),

  getters: {
    isEmpty: state => !state.loading && !state.error && state.items.length === 0,
    pendingCount: state => state.items.filter(item => item.status === 'PENDING').length,
    pageCount: state => Math.max(1, Math.ceil(state.total / state.pageSize))
  },

  actions: {
    params() {
      return {
        status: this.filters.status === null ? undefined : this.filters.status,
        page: this.page,
        pageSize: this.pageSize
      }
    },

    async load() {
      this.loading = true
      this.error = ''
      try {
        const data = await fetchAppeals(this.params())
        this.items = Array.isArray(data.items) ? data.items : []
        this.total = toInteger(data.total) ?? this.items.length
        this.page = toInteger(data.page) ?? this.page
        return true
      } catch (error) {
        this.items = []
        this.total = 0
        this.error = error?.userMessage || '申诉队列加载失败，请稍后重试'
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
      this.filters = { status: null }
      this.page = 1
      return this.load()
    },

    goToPage(page) {
      const target = toInteger(page)
      if (target === null || target < 1) return Promise.resolve(false)
      this.page = Math.min(target, this.pageCount)
      return this.load()
    },

    /**
     * 审核驳回：申诉转 REJECTED 并记录处理意见。订单保持完成、钱包不动 ——
     * 以前队列里只有"通过"，一条站不住脚的申诉只能靠退款才能关掉。
     */
    async reject(appeal, reason) {
      this.saving = true
      this.error = ''
      this.notice = ''
      try {
        const data = await rejectAppeal(appeal.id, reason)
        this.items = this.items.map(item =>
          item.id === appeal.id
            ? {
                ...item,
                status: data.status || 'REJECTED',
                decidedAt: data.decidedAt ?? item.decidedAt,
                decisionReason: data.decisionReason ?? reason
              }
            : item
        )
        this.notice = `申诉 #${appeal.id} 已驳回，订单与钱包保持不变`
        return true
      } catch (error) {
        this.error = error?.userMessage || '驳回失败，请稍后重试'
        return false
      } finally {
        this.saving = false
      }
    },

    /** 审核通过：申诉转 APPROVED、订单取消、实付金额退回钱包并写审计（服务端同事务）。 */
    async approve(appeal) {
      this.saving = true
      this.error = ''
      this.notice = ''
      try {
        const data = await approveAppeal(appeal.id)
        this.items = this.items.map(item =>
          item.id === appeal.id
            ? { ...item, status: data.status || 'APPROVED', decidedAt: data.decidedAt ?? item.decidedAt }
            : item
        )
        this.notice = `申诉 #${appeal.id} 已审核通过，实付 ${appeal.orderPaidCent} 分将退回用户钱包`
        return true
      } catch (error) {
        this.error = error?.userMessage || '审核失败，请稍后重试'
        return false
      } finally {
        this.saving = false
      }
    },

    clearMessages() {
      this.error = ''
      this.notice = ''
    }
  }
})
