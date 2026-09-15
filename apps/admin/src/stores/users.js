import { defineStore } from 'pinia'
import { fetchUser, fetchUserOrders, fetchUsers, setUserStatus } from '../api/user'
import { ERROR_CODES } from '../api/http'
import { toInteger } from '../utils/format'
import { useAuthStore } from './auth'

/**
 * 用户管理状态（接口文档 §6.4–§6.7）。
 * 手机号过滤只支持完整 11 位精确匹配或后四位匹配，接口侧不提供模糊扫描，
 * 因此这里在发起请求前先做本地校验，避免无意义的失败请求。
 */

/** 手机号过滤白名单（与后端正则一致）。 */
const PHONE_EXACT_PATTERN = /^[0-9]{11}$/
const PHONE_LAST4_PATTERN = /^[0-9]{4}$/

/** 排序白名单 §6.4。 */
export const USER_SORTS = [
  { value: '-registeredAt', label: '注册时间（新→旧）' },
  { value: 'registeredAt', label: '注册时间（旧→新）' },
  { value: '-balanceCent', label: '余额（高→低）' },
  { value: 'balanceCent', label: '余额（低→高）' }
]

export const useUsersStore = defineStore('adminUsers', {
  state: () => ({
    items: [],
    total: 0,
    page: 1,
    pageSize: 20,
    filters: { status: null, phoneExact: '', phoneLast4: '', sort: '-registeredAt' },
    loading: false,
    saving: false,
    error: '',
    notice: '',
    conflict: '',
    detail: null,
    detailLoading: false,
    detailError: '',
    orders: [],
    ordersTotal: 0,
    ordersLoading: false,
    ordersError: '',
    /** 冻结时保留的进行中流程提示（服务端返回 activeFlowPreserved）。 */
    activeFlowPreserved: false
  }),

  getters: {
    isEmpty: state => !state.loading && !state.error && state.items.length === 0,
    selectedUser: state => state.detail,
    hasDetail: state => state.detail !== null,
    pageCount: state => Math.max(1, Math.ceil(state.total / state.pageSize))
  },

  actions: {
    /** 过滤参数校验：返回错误文案或空字符串。 */
    validateFilters() {
      const exact = this.filters.phoneExact.trim()
      const last4 = this.filters.phoneLast4.trim()
      if (exact !== '' && !PHONE_EXACT_PATTERN.test(exact)) return '完整手机号必须是 11 位数字'
      if (last4 !== '' && !PHONE_LAST4_PATTERN.test(last4)) return '手机号后四位必须是 4 位数字'
      if (exact !== '' && last4 !== '') return '完整手机号与后四位只能选择一种查询方式'
      return ''
    },

    params() {
      return {
        status: this.filters.status === null ? undefined : this.filters.status,
        phoneExact: this.filters.phoneExact.trim() || undefined,
        phoneLast4: this.filters.phoneLast4.trim() || undefined,
        sort: this.filters.sort || undefined,
        page: this.page,
        pageSize: this.pageSize
      }
    },

    async load() {
      const invalid = this.validateFilters()
      if (invalid) {
        this.items = []
        this.total = 0
        this.error = invalid
        return false
      }
      this.loading = true
      this.error = ''
      try {
        const data = await fetchUsers(this.params())
        this.items = Array.isArray(data.items) ? data.items : []
        this.total = toInteger(data.total) ?? this.items.length
        this.page = toInteger(data.page) ?? this.page
        return true
      } catch (error) {
        this.items = []
        this.total = 0
        this.error = error?.userMessage || '用户列表加载失败，请稍后重试'
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
      this.filters = { status: null, phoneExact: '', phoneLast4: '', sort: '-registeredAt' }
      this.page = 1
      return this.load()
    },

    goToPage(page) {
      const target = toInteger(page)
      if (target === null || target < 1) return Promise.resolve(false)
      this.page = Math.min(target, this.pageCount)
      return this.load()
    },

    /** §6.5 用户详情（含会话数量与活动流程摘要）。 */
    async loadDetail(userId) {
      this.detailLoading = true
      this.detailError = ''
      try {
        this.detail = await fetchUser(userId)
        return this.detail
      } catch (error) {
        this.detail = null
        this.detailError = error?.userMessage || '用户详情加载失败'
        return null
      } finally {
        this.detailLoading = false
      }
    },

    closeDetail() {
      this.detail = null
      this.detailError = ''
      this.orders = []
      this.ordersTotal = 0
      this.ordersError = ''
    },

    /** §6.7 用户订单历史（管理员访问会写审计日志）。 */
    async loadOrders(userId, { page = 1, status = null } = {}) {
      this.ordersLoading = true
      this.ordersError = ''
      try {
        const data = await fetchUserOrders(userId, {
          page,
          pageSize: 20,
          status: status === null ? undefined : status
        })
        this.orders = Array.isArray(data.items) ? data.items : []
        this.ordersTotal = toInteger(data.total) ?? this.orders.length
        return this.orders
      } catch (error) {
        this.orders = []
        this.ordersTotal = 0
        this.ordersError = error?.userMessage || '订单历史加载失败'
        return []
      } finally {
        this.ordersLoading = false
      }
    },

    /** §6.6 冻结或解冻：失败时若版本冲突则重载并提示。 */
    async setStatus(user, status, reason) {
      const auth = useAuthStore()
      this.saving = true
      this.error = ''
      this.notice = ''
      this.conflict = ''
      try {
        const data = await auth.runWithReauth(({ idempotencyKey }) =>
          setUserStatus(user.id, { status, reason, version: user.version }, { idempotencyKey })
        )
        this.activeFlowPreserved = data.activeFlowPreserved === true
        this.items = this.items.map(item =>
          item.id === user.id
            ? { ...item, status: data.status, statusText: data.statusText, version: toInteger(data.version) ?? item.version }
            : item
        )
        if (this.detail && this.detail.id === user.id) {
          this.detail = {
            ...this.detail,
            status: data.status,
            statusText: data.statusText,
            version: toInteger(data.version) ?? this.detail.version
          }
        }
        this.notice =
          data.status === 0
            ? `用户 #${user.id} 已冻结${this.activeFlowPreserved ? '，进行中的充电流程继续由服务端结算' : ''}`
            : `用户 #${user.id} 已解冻`
        return true
      } catch (error) {
        if (error?.code === ERROR_CODES.VERSION_CONFLICT) {
          this.conflict = '该用户资料已被其他管理员更新，请刷新后重试'
          await this.load()
          return false
        }
        this.error = error?.userMessage || '冻结或解冻失败'
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
