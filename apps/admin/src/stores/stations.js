import { defineStore } from 'pinia'
import {
  createPriceAdjustment,
  createStation,
  createTariff,
  fetchStations,
  fetchTariffs,
  setStationEnabled,
  updateStation
} from '../api/station'
import { ERROR_CODES } from '../api/http'
import { toInteger } from '../utils/format'
import { useAuthStore } from './auth'

/**
 * 站点与价格版本状态（接口文档 §7.1–§7.5、§7.11–§7.13）。
 *
 * 写入策略：
 * - 启停、修改状态这类小改动用服务端返回体就地更新行（乐观合并），不整表重载；
 * - 新增/批量这类结构性变更成功后触发一次 reload，保证列表与总数一致；
 * - VERSION_CONFLICT 一律提示并重载，绝不覆盖其他管理员刚提交的版本。
 */
export const useStationsStore = defineStore('adminStations', {
  state: () => ({
    items: [],
    total: 0,
    page: 1,
    pageSize: 20,
    filters: { status: null, adcode: '', keyword: '' },
    loading: false,
    saving: false,
    error: '',
    notice: '',
    conflict: '',
    selectedStationId: null,
    tariffs: [],
    tariffTotal: 0,
    tariffPage: 1,
    tariffPageSize: 10,
    tariffsLoading: false,
    tariffsError: ''
  }),

  getters: {
    isEmpty: state => !state.loading && !state.error && state.items.length === 0,
    selectedStation: state => state.items.find(item => item.id === state.selectedStationId) || null,
    enabledCount: state => state.items.filter(item => item.enabled === true).length,
    pageCount: state => Math.max(1, Math.ceil(state.total / state.pageSize))
  },

  actions: {
    /** 查询参数：空字符串按未提供处理（§1.5）。 */
    params() {
      return {
        status: this.filters.status === null ? undefined : this.filters.status,
        adcode: this.filters.adcode.trim() || undefined,
        keyword: this.filters.keyword.trim() || undefined,
        page: this.page,
        pageSize: this.pageSize
      }
    },

    async load() {
      this.loading = true
      this.error = ''
      try {
        const data = await fetchStations(this.params())
        this.items = Array.isArray(data.items) ? data.items : []
        this.total = toInteger(data.total) ?? this.items.length
        this.page = toInteger(data.page) ?? this.page
        this.pageSize = toInteger(data.pageSize) ?? this.pageSize
        return true
      } catch (error) {
        this.items = []
        this.total = 0
        this.error = error?.userMessage || '站点列表加载失败，请稍后重试'
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
      this.filters = { status: null, adcode: '', keyword: '' }
      this.page = 1
      return this.load()
    },

    goToPage(page) {
      const target = toInteger(page)
      if (target === null || target < 1) return Promise.resolve(false)
      this.page = Math.min(target, this.pageCount)
      return this.load()
    },

    select(stationId) {
      this.selectedStationId = stationId
    },

    /** 写入失败的统一处理：版本冲突单独提示并重载。 */
    async handleWriteError(error) {
      if (error?.code === ERROR_CODES.VERSION_CONFLICT) {
        this.conflict = '站点已被其他管理员修改，已刷新为最新版本，请核对后重试'
        await this.load()
        return false
      }
      this.error = error?.userMessage || '操作失败，请稍后重试'
      return false
    },

    /** §7.2 新增站点（含初始设备组合创建）。 */
    async create(payload) {
      const auth = useAuthStore()
      this.saving = true
      this.error = ''
      this.notice = ''
      this.conflict = ''
      try {
        const data = await auth.runWithReauth(({ idempotencyKey }) => createStation(payload, { idempotencyKey }))
        this.notice = `站点已创建（编号 ${data.code || data.id}）`
        await this.load()
        return true
      } catch (error) {
        return await this.handleWriteError(error)
      } finally {
        this.saving = false
      }
    },

    /** §7.3 修改站点：只提交变更字段与当前 version。 */
    async edit(station, patch) {
      const auth = useAuthStore()
      this.saving = true
      this.error = ''
      this.notice = ''
      this.conflict = ''
      try {
        const data = await auth.runWithReauth(({ idempotencyKey }) =>
          updateStation(station.id, { ...patch, version: station.version }, { idempotencyKey })
        )
        this.mergeRow(station.id, { version: toInteger(data.version) ?? station.version })
        this.notice = '站点信息已更新'
        return true
      } catch (error) {
        return await this.handleWriteError(error)
      } finally {
        this.saving = false
      }
    },

    /** §7.4/§7.5 启用或停用站点：成功后用返回体就地更新该行。 */
    async setEnabled(station, enabled, reason) {
      const auth = useAuthStore()
      this.saving = true
      this.error = ''
      this.notice = ''
      this.conflict = ''
      try {
        const data = await auth.runWithReauth(({ idempotencyKey }) =>
          setStationEnabled(station.id, enabled, { reason, version: station.version, idempotencyKey })
        )
        this.mergeRow(station.id, {
          enabled: data.enabled === true,
          version: toInteger(data.version) ?? station.version
        })
        this.notice = enabled ? '站点已启用' : '站点已停用'
        return true
      } catch (error) {
        return await this.handleWriteError(error)
      } finally {
        this.saving = false
      }
    },

    mergeRow(stationId, patch) {
      this.items = this.items.map(item => (item.id === stationId ? { ...item, ...patch } : item))
    },

    /** §7.11 基础价格版本列表。 */
    async loadTariffs() {
      this.tariffsLoading = true
      this.tariffsError = ''
      try {
        const data = await fetchTariffs({
          adcode: this.filters.adcode.trim() || undefined,
          page: this.tariffPage,
          pageSize: this.tariffPageSize
        })
        this.tariffs = Array.isArray(data.items) ? data.items : []
        this.tariffTotal = toInteger(data.total) ?? this.tariffs.length
        return true
      } catch (error) {
        this.tariffs = []
        this.tariffTotal = 0
        this.tariffsError = error?.userMessage || '价格版本加载失败'
        return false
      } finally {
        this.tariffsLoading = false
      }
    },

    /** §7.12 创建基础价格版本（敏感操作：先试后验证）。 */
    async createTariffVersion(payload) {
      const auth = useAuthStore()
      this.saving = true
      this.error = ''
      this.notice = ''
      try {
        await auth.runWithReauth(({ idempotencyKey }) => createTariff(payload, { idempotencyKey }))
        this.notice = '基础价格版本已创建'
        await this.loadTariffs()
        return true
      } catch (error) {
        this.error = error?.userMessage || '创建价格版本失败'
        return false
      } finally {
        this.saving = false
      }
    },

    /** §7.13 批准服务费调整。 */
    async approveAdjustment(payload) {
      const auth = useAuthStore()
      this.saving = true
      this.error = ''
      this.notice = ''
      try {
        const data = await auth.runWithReauth(({ idempotencyKey }) =>
          createPriceAdjustment(payload, { idempotencyKey })
        )
        this.notice = `服务费调整已批准（${data.adjustmentBp > 0 ? '+' : ''}${data.adjustmentBp} bp）`
        return true
      } catch (error) {
        this.error = error?.userMessage || '服务费调整失败'
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
