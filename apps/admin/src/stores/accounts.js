import { defineStore } from 'pinia'
import { createAccount, fetchAccounts, setAccountStatus } from '../api/account'
import { ERROR_CODES } from '../api/http'
import { toInteger } from '../utils/format'
import { useAuthStore } from './auth'

/**
 * 管理员账号状态（接口文档 §6.8–§6.10）。
 * 列表与写入都需要 OWNER 权限；创建与停启用还需要重新验证（服务端返回 REAUTH_REQUIRED
 * 时由 auth store 弹窗后原幂等键重试）。
 */
export const useAccountsStore = defineStore('adminAccounts', {
  state: () => ({
    items: [],
    total: 0,
    page: 1,
    pageSize: 20,
    loading: false,
    saving: false,
    error: '',
    notice: '',
    conflict: ''
  }),

  getters: {
    isEmpty: state => !state.loading && !state.error && state.items.length === 0,
    enabledCount: state => state.items.filter(item => item.status === 1).length,
    pendingPasswordCount: state => state.items.filter(item => item.mustChangePassword === true).length,
    pageCount: state => Math.max(1, Math.ceil(state.total / state.pageSize))
  },

  actions: {
    async load() {
      this.loading = true
      this.error = ''
      try {
        const data = await fetchAccounts({ page: this.page, pageSize: this.pageSize })
        this.items = Array.isArray(data.items) ? data.items : []
        this.total = toInteger(data.total) ?? this.items.length
        this.page = toInteger(data.page) ?? this.page
        return true
      } catch (error) {
        this.items = []
        this.total = 0
        this.error = error?.userMessage || '管理员账号加载失败（需要 OWNER 权限）'
        return false
      } finally {
        this.loading = false
      }
    },

    goToPage(page) {
      const target = toInteger(page)
      if (target === null || target < 1) return Promise.resolve(false)
      this.page = Math.min(target, this.pageCount)
      return this.load()
    },

    /** §6.9 创建运营管理员：角色固定 OPERATOR，首次登录必须改密。 */
    async create({ username, password, reason }) {
      const auth = useAuthStore()
      this.saving = true
      this.error = ''
      this.notice = ''
      this.conflict = ''
      try {
        const data = await auth.runWithReauth(({ idempotencyKey }) =>
          createAccount({ username, password, reason }, { idempotencyKey })
        )
        this.notice = `管理员账号 ${data.username || username} 已创建，首次登录需修改密码`
        await this.load()
        return true
      } catch (error) {
        this.error = error?.userMessage || '创建管理员账号失败'
        return false
      } finally {
        this.saving = false
      }
    },

    /** §6.10 停用或启用：OWNER 不得停用本人，服务端会返回 VALIDATION_FAILED。 */
    async setStatus(account, status, reason) {
      const auth = useAuthStore()
      this.saving = true
      this.error = ''
      this.notice = ''
      this.conflict = ''
      try {
        const data = await auth.runWithReauth(({ idempotencyKey }) =>
          setAccountStatus(account.id, { status, reason, version: account.version }, { idempotencyKey })
        )
        this.items = this.items.map(item =>
          item.id === account.id
            ? { ...item, status: data.status, version: toInteger(data.version) ?? item.version }
            : item
        )
        this.notice = data.status === 0 ? `账号 ${account.username} 已停用` : `账号 ${account.username} 已启用`
        return true
      } catch (error) {
        if (error?.code === ERROR_CODES.VERSION_CONFLICT) {
          this.conflict = '该账号已被其他管理员修改，已刷新为最新状态'
          await this.load()
          return false
        }
        this.error = error?.userMessage || '账号状态变更失败'
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
