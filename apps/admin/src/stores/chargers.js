import { defineStore } from 'pinia'
import {
  createChargersBatch,
  createRestartCommand,
  fetchChargers,
  fetchDeviceCommand,
  setChargerStatus
} from '../api/charger'
import { fetchStations } from '../api/station'
import { isoToUnixSecond } from '../api/contract'
import { ERROR_CODES } from '../api/http'
import { toInteger } from '../utils/format'
import { COMMAND_STATUS } from '../utils/domain'
import { useAuthStore } from './auth'

/** 重启命令轮询间隔与上限：两秒模拟执行期，15 次足以覆盖服务端完成或超时。 */
export const COMMAND_POLL_INTERVAL_MS = 1000
export const COMMAND_POLL_MAX_ATTEMPTS = 15

let pollTimer = null

/**
 * 设备状态（接口文档 §7.6–§7.10）。
 * 重启是异步命令：创建成功后按命令编号轮询状态，直到进入终态或达到轮询上限。
 */
export const useChargersStore = defineStore('adminChargers', {
  state: () => ({
    items: [],
    total: 0,
    page: 1,
    pageSize: 20,
    filters: { stationId: null, status: null },
    loading: false,
    saving: false,
    error: '',
    notice: '',
    conflict: '',
    stationOptions: [],
    selectedChargerId: null,
    /** 最近一次远程重启命令与其轮询状态。 */
    command: null,
    commandPolling: false,
    commandAttempts: 0
  }),

  getters: {
    isEmpty: state => !state.loading && !state.error && state.items.length === 0,
    selectedCharger: state => state.items.find(item => item.id === state.selectedChargerId) || null,
    commandTone: state => COMMAND_STATUS[state.command?.status]?.tone || 'muted',
    commandLabel: state => COMMAND_STATUS[state.command?.status]?.label || '—',
    commandFinished: state => COMMAND_STATUS[state.command?.status]?.terminal === true,
    pageCount: state => Math.max(1, Math.ceil(state.total / state.pageSize))
  },

  actions: {
    params() {
      // Go 契约仅支持 stationId/status 过滤；keyword/chargerType 在契约补齐前不提交。
      return {
        stationId: this.filters.stationId === null ? undefined : this.filters.stationId,
        status: this.filters.status === null ? undefined : this.filters.status,
        page: this.page,
        pageSize: this.pageSize
      }
    },

    async load() {
      this.loading = true
      this.error = ''
      try {
        const data = await fetchChargers(this.params())
        this.items = Array.isArray(data.items) ? data.items : []
        this.total = toInteger(data.total) ?? this.items.length
        this.page = toInteger(data.page) ?? this.page
        return true
      } catch (error) {
        this.items = []
        this.total = 0
        this.error = error?.userMessage || '设备列表加载失败，请稍后重试'
        return false
      } finally {
        this.loading = false
      }
    },

    /** 站点下拉选项：只取第一页 100 条，够用且不引入新接口。 */
    async loadStationOptions() {
      try {
        const data = await fetchStations({ page: 1, pageSize: 100 })
        this.stationOptions = Array.isArray(data.items) ? data.items : []
        return this.stationOptions
      } catch {
        this.stationOptions = []
        return []
      }
    },

    setFilter(patch) {
      Object.assign(this.filters, patch)
      this.page = 1
      return this.load()
    },

    resetFilters() {
      this.filters = { stationId: null, status: null }
      this.page = 1
      return this.load()
    },

    goToPage(page) {
      const target = toInteger(page)
      if (target === null || target < 1) return Promise.resolve(false)
      this.page = Math.min(target, this.pageCount)
      return this.load()
    },

    select(chargerId) {
      this.selectedChargerId = chargerId
    },

    async handleWriteError(error) {
      if (error?.code === ERROR_CODES.VERSION_CONFLICT) {
        this.conflict = '设备已被其他管理员修改，已刷新为最新状态，请核对后重试'
        await this.load()
        return false
      }
      this.error = error?.userMessage || '操作失败，请稍后重试'
      return false
    },

    /** §7.8 设置设备状态：成功后用返回体就地更新该行，不整表重载。 */
    async setStatus(charger, targetStatus, reason) {
      const auth = useAuthStore()
      this.saving = true
      this.error = ''
      this.notice = ''
      this.conflict = ''
      try {
        const data = await auth.runWithReauth(({ idempotencyKey }) =>
          setChargerStatus(charger.id, { targetStatus, reason, version: charger.version }, { idempotencyKey })
        )
        this.mergeRow(charger.id, {
          status: data.status,
          statusText: data.statusText,
          version: toInteger(data.version) ?? charger.version
        })
        this.notice = `设备 ${charger.code} 状态已更新为${data.statusText || targetStatus}`
        return true
      } catch (error) {
        return await this.handleWriteError(error)
      } finally {
        this.saving = false
      }
    },

    /** §7.7 批量创建设备：一次最多 100 台，成功后重载列表。 */
    async batchCreate({ stationId, chargers }) {
      const auth = useAuthStore()
      this.saving = true
      this.error = ''
      this.notice = ''
      try {
        const data = await auth.runWithReauth(({ idempotencyKey }) =>
          createChargersBatch({ stationId, chargers }, { idempotencyKey })
        )
        const created = Array.isArray(data.created) ? data.created : []
        const count = toInteger(data.chargerCount) ?? created.length
        this.notice = `已创建设备 ${count} 台`
        await this.load()
        return true
      } catch (error) {
        this.error = error?.userMessage || '批量创建设备失败'
        return false
      } finally {
        this.saving = false
      }
    },

    /** 远程重启（Go 契约）：202 返回 {commandId, status}，受理后轮询命令回执结果。 */
    async restart(charger, reason) {
      const auth = useAuthStore()
      this.saving = true
      this.error = ''
      this.notice = ''
      try {
        const data = await auth.runWithReauth(({ idempotencyKey }) =>
          createRestartCommand(charger.id, { reason }, { idempotencyKey })
        )
        this.command = {
          commandId: data.commandId || '',
          status: data.status || 'PENDING',
          chargerStatus: null,
          // 契约的受理响应只有 commandId + status（backend Command 结构），
          // 没有受理时间：以前这里写 createdAt: toInteger(data.createdAt) ?? 0，
          // 页面就会渲染成"提交于 —"。宁可不说，也不编一个 1970。
          completedAt: null,
          errorSummary: '',
          chargerCode: charger.code
        }
        this.notice = `重启指令已受理（${data.commandId || '无编号'}），正在等待设备回执`
        if (this.command.commandId) this.startPolling()
        return true
      } catch (error) {
        this.error = error?.userMessage || '远程重启失败'
        return false
      } finally {
        this.saving = false
      }
    },

    /**
     * 查询一次命令回执（GET /admin/device-commands/{commandId}）。
     * Go 契约：回执未到达返回 404 —— 按"仍在等待"处理；回执到达后 result
     * COMPLETED→SUCCEEDED、FAILED→FAILED，终态时停止轮询并刷新设备列表。
     */
    async pollCommand() {
      if (!this.command?.commandId) return null
      let data
      try {
        data = await fetchDeviceCommand(this.command.commandId)
      } catch (error) {
        if (error?.status === 404) return this.command
        throw error
      }
      this.command = {
        ...this.command,
        status: data.result === 'COMPLETED' ? 'SUCCEEDED' : data.result === 'FAILED' ? 'FAILED' : this.command.status,
        completedAt: isoToUnixSecond(data.recordedAt),
        errorSummary: data.result === 'FAILED' ? '设备回执报告重启失败' : ''
      }
      if (COMMAND_STATUS[this.command.status]?.terminal) {
        this.stopPolling()
        await this.load()
      }
      return this.command
    },

    startPolling() {
      this.stopPolling()
      this.commandPolling = true
      this.commandAttempts = 0
      const tick = async () => {
        if (!this.commandPolling) return
        this.commandAttempts += 1
        try {
          await this.pollCommand()
        } catch (error) {
          this.command = { ...this.command, errorSummary: error?.userMessage || '命令状态查询失败' }
        }
        if (!this.commandPolling || this.commandFinished) return
        if (this.commandAttempts >= COMMAND_POLL_MAX_ATTEMPTS) {
          this.stopPolling()
          this.notice = `命令 ${this.command?.commandNo} 仍在处理中，请稍后刷新设备状态`
          return
        }
        pollTimer = setTimeout(tick, COMMAND_POLL_INTERVAL_MS)
      }
      pollTimer = setTimeout(tick, COMMAND_POLL_INTERVAL_MS)
    },

    stopPolling() {
      if (pollTimer) clearTimeout(pollTimer)
      pollTimer = null
      this.commandPolling = false
    },

    mergeRow(chargerId, patch) {
      this.items = this.items.map(item => (item.id === chargerId ? { ...item, ...patch } : item))
    },

    clearMessages() {
      this.error = ''
      this.notice = ''
      this.conflict = ''
    }
  }
})
