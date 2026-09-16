import { defineStore } from 'pinia'
import {
  createPriceAdjustment,
  createStation,
  fetchStations,
  fetchTariffs,
  setGlobalTariff,
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
    filters: { keyword: '' },
    loading: false,
    saving: false,
    error: '',
    notice: '',
    conflict: '',
    selectedStationId: null,
    /** 全局费率：每条配置一行，外加整支车队的汇总。 */
    tariffs: [],
    tariffSummary: null,
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
    /**
     * 查询参数：空字符串按未提供处理（§1.5）。
     * Go 列表契约只有 keyword + 分页，所以这里也只产出这两项——
     * 不再保留 status/adcode 这类服务端不认的字段，避免“看起来生效了”。
     */
    params() {
      return {
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
      this.filters = { keyword: '' }
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

    /**
     * §7.3 修改站点：只提交名称、地址与经纬度。
     *
     * 服务端返回整条记录，就地合并这四项——包括编码与状态在内的其他字段不在此
     * 端点的职责内，直接覆盖整行反而会把别处刚改的字段写回旧值。
     */
    async edit(station, patch) {
      this.saving = true
      this.error = ''
      this.notice = ''
      this.conflict = ''
      try {
        const data = await updateStation(station.id, patch)
        this.mergeRow(station.id, {
          name: data?.name ?? patch.name,
          address: data?.address ?? patch.address,
          latitudeE6: data?.latitudeE6 ?? patch.latitudeE6,
          longitudeE6: data?.longitudeE6 ?? patch.longitudeE6
        })
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

    /**
     * §7.11 全局费率：所有电桩当前的费率构成。
     *
     * 每行带一个稳定 key：返回的是「配置」而不是「记录」，同一组价格可能重复出现，
     * 用价格字段当行键会在两行相同时让表格错位。
     */
    async loadTariffs() {
      this.tariffsLoading = true
      this.tariffsError = ''
      try {
        const data = await fetchTariffs()
        const configurations = Array.isArray(data?.configurations) ? data.configurations : []
        this.tariffs = configurations.map(configuration => ({
          ...configuration,
          key: `${configuration.electricityPriceCentPerKwh}-${configuration.servicePriceCentPerKwh}` +
            `-${configuration.offPeakElectricityPriceCentPerKwh ?? 'flat'}` +
            `-${configuration.offPeakStartHour ?? ''}-${configuration.offPeakEndHour ?? ''}`
        }))
        this.tariffSummary = {
          chargerCount: toInteger(data?.chargerCount) ?? 0,
          configurationCount: toInteger(data?.configurationCount) ?? this.tariffs.length,
          minElectricityPriceCentPerKwh: toInteger(data?.minElectricityPriceCentPerKwh) ?? 0,
          maxElectricityPriceCentPerKwh: toInteger(data?.maxElectricityPriceCentPerKwh) ?? 0,
          minServicePriceCentPerKwh: toInteger(data?.minServicePriceCentPerKwh) ?? 0,
          maxServicePriceCentPerKwh: toInteger(data?.maxServicePriceCentPerKwh) ?? 0,
          offPeakChargerCount: toInteger(data?.offPeakChargerCount) ?? 0,
          flatChargerCount: toInteger(data?.flatChargerCount) ?? 0
        }
        return true
      } catch (error) {
        this.tariffs = []
        this.tariffSummary = null
        this.tariffsError = error?.userMessage || '费率加载失败'
        return false
      } finally {
        this.tariffsLoading = false
      }
    },

    /** §7.12 全局下发费率：一次写入所有电桩，原因必填并写审计。 */
    async setGlobalTariff(payload) {
      this.saving = true
      this.error = ''
      this.notice = ''
      try {
        const data = await setGlobalTariff(payload)
        const affected = toInteger(data?.affectedChargers) ?? 0
        const replaced = toInteger(data?.previousConfigurations) ?? 0
        this.notice = replaced > 1
          ? `费率已下发到 ${affected} 台设备（原 ${replaced} 种费率被统一）`
          : `费率已确认：${affected} 台设备`
        await this.loadTariffs()
        return true
      } catch (error) {
        this.error = error?.userMessage || '费率下发失败'
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
