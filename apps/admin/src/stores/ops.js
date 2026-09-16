import { defineStore } from 'pinia'
import { createBackup, fetchAuditLogs, fetchBackups, verifyBackup } from '../api/ops'
import { toInteger } from '../utils/format'
import { useAuthStore } from './auth'

/**
 * 运维状态：审计日志与备份（接口文档 §8.5–§8.8）。
 * 审计日志已迁到 Go（只读）；备份域 Go 契约暂未提供，api/ops.js 直接抛 unsupported。
 * 创建备份与隔离恢复验证属于敏感操作，先试后重新验证。
 */
export const useOpsStore = defineStore('adminOps', {
  state: () => ({
    audit: { items: [], page: 1, pageSize: 20 },
    auditFilters: { actorId: '', action: '', targetType: '', targetId: '', fromAt: '', toAt: '' },
    auditLoading: false,
    auditError: '',
    backups: [],
    backupsLoading: false,
    backupsError: '',
    saving: false,
    error: '',
    notice: '',
    /** 最近一次备份验证结果：{backupNo, verificationStatus}。 */
    verification: null
  }),

  getters: {
    auditEmpty: state => !state.auditLoading && !state.auditError && state.audit.items.length === 0,
    backupsEmpty: state => !state.backupsLoading && !state.backupsError && state.backups.length === 0,
    latestBackup: state => state.backups[0] || null,
    verifiedCount: state => state.backups.filter(item => item.verificationStatus === 'SUCCEEDED').length
  },

  actions: {
    auditParams() {
      const actor = this.auditFilters.actorId.trim()
      return {
        actorId: actor === '' ? undefined : actor,
        action: this.auditFilters.action.trim() || undefined,
        targetType: this.auditFilters.targetType.trim() || undefined,
        targetId: this.auditFilters.targetId.trim() || undefined,
        // 时间范围在视图层由本地时间输入换算为 UTC 秒后写入过滤器。
        fromAt: toInteger(this.auditFilters.fromAt) || undefined,
        toAt: toInteger(this.auditFilters.toAt) || undefined,
        page: this.audit.page,
        pageSize: this.audit.pageSize
      }
    },

    /** §8.5 审计日志：接口只返回 items/page/pageSize，没有 total。 */
    async loadAuditLogs() {
      this.auditLoading = true
      this.auditError = ''
      try {
        const data = await fetchAuditLogs(this.auditParams())
        this.audit.items = Array.isArray(data.items) ? data.items : []
        this.audit.page = toInteger(data.page) ?? this.audit.page
        this.audit.pageSize = toInteger(data.pageSize) ?? this.audit.pageSize
        return true
      } catch (error) {
        this.audit.items = []
        this.auditError = error?.userMessage || '审计日志加载失败，请稍后重试'
        return false
      } finally {
        this.auditLoading = false
      }
    },

    setAuditFilter(patch) {
      Object.assign(this.auditFilters, patch)
      this.audit.page = 1
      return this.loadAuditLogs()
    },

    resetAuditFilters() {
      this.auditFilters = { actorId: '', action: '', targetType: '', targetId: '', fromAt: '', toAt: '' }
      this.audit.page = 1
      return this.loadAuditLogs()
    },

    goToAuditPage(page) {
      const target = toInteger(page)
      if (target === null || target < 1) return Promise.resolve(false)
      this.audit.page = target
      return this.loadAuditLogs()
    },

    /** §8.7 备份记录列表。 */
    async loadBackups() {
      this.backupsLoading = true
      this.backupsError = ''
      try {
        const data = await fetchBackups()
        this.backups = Array.isArray(data.items) ? data.items : []
        return true
      } catch (error) {
        this.backups = []
        this.backupsError = error?.userMessage || '备份记录加载失败，请稍后重试'
        return false
      } finally {
        this.backupsLoading = false
      }
    },

    /** §8.6 创建一致性备份：必须二次确认后才调用。 */
    async createBackup() {
      const auth = useAuthStore()
      this.saving = true
      this.error = ''
      this.notice = ''
      try {
        const data = await auth.runWithReauth(({ idempotencyKey }) => createBackup({ idempotencyKey }))
        this.notice = `备份任务已创建（${data.backupNo || '无编号'}）`
        await this.loadBackups()
        return true
      } catch (error) {
        this.error = error?.userMessage || '创建备份失败'
        return false
      } finally {
        this.saving = false
      }
    },

    /** §8.8 隔离恢复验证。 */
    async verify(backupNo) {
      const auth = useAuthStore()
      this.saving = true
      this.error = ''
      this.notice = ''
      try {
        const data = await auth.runWithReauth(({ idempotencyKey }) => verifyBackup(backupNo, { idempotencyKey }))
        this.verification = {
          backupNo: data.backupNo || backupNo,
          verificationStatus: data.verificationStatus || 'PENDING'
        }
        this.notice = `备份 ${backupNo} 验证已提交（${this.verification.verificationStatus}）`
        await this.loadBackups()
        return true
      } catch (error) {
        this.error = error?.userMessage || '备份验证失败'
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
