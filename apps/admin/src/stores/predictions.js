import { defineStore } from 'pinia'
import { fetchMlTask, fetchPredictions, startMlTask } from '../api/ml'
import { fetchStations } from '../api/station'
import { toInteger } from '../utils/format'
import { ML_TASK_STATUS } from '../utils/domain'

/** ML 任务轮询参数：预测为秒级任务，1 秒一次、最多 20 次。 */
export const ML_POLL_INTERVAL_MS = 1000
export const ML_POLL_MAX_ATTEMPTS = 20

let pollTimer = null

/**
 * 智能预测状态（接口文档 §9.1–§9.3）。
 *
 * 触发入口是 §9.2 的 POST /admin/ml-tasks（任务类型 PREDICT/TRAIN）。
 * 服务端没有提供“任务列表”接口，因此管理端只轮询自己刚启动的任务编号，
 * 不展示也无法展示历史任务列表（见 api/ml.js 的说明）。
 */
export const usePredictionsStore = defineStore('adminPredictions', {
  state: () => ({
    items: [],
    filters: { stationId: null, horizonHour: null },
    loading: false,
    error: '',
    notice: '',
    stationOptions: [],
    /** 当前任务：{taskNo, taskType, status, modelVersion}。 */
    task: null,
    taskPolling: false,
    taskAttempts: 0,
    taskError: '',
    lastLoadedAt: 0
  }),

  getters: {
    isEmpty: state => !state.loading && !state.error && state.items.length === 0,
    taskTone: state => ML_TASK_STATUS[state.task?.status]?.tone || 'muted',
    taskLabel: state => ML_TASK_STATUS[state.task?.status]?.label || '—',
    taskFinished: state => ML_TASK_STATUS[state.task?.status]?.terminal === true,
    peakCount: state => state.items.filter(item => item.peakFlag === true).length,
    staleCount: state => state.items.filter(item => item.staleFlag === true).length
  },

  actions: {
    params() {
      return {
        stationId: this.filters.stationId === null ? undefined : this.filters.stationId,
        horizonHour: this.filters.horizonHour === null ? undefined : this.filters.horizonHour
      }
    },

    async load() {
      this.loading = true
      this.error = ''
      try {
        const data = await fetchPredictions(this.params())
        this.items = Array.isArray(data.items) ? data.items : []
        this.lastLoadedAt = Math.floor(Date.now() / 1000)
        return true
      } catch (error) {
        this.items = []
        this.error = error?.userMessage || '预测结果加载失败，请稍后重试'
        return false
      } finally {
        this.loading = false
      }
    },

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
      return this.load()
    },

    /** §9.2 启动预测任务（周期固定为 1/6/24 小时，与查询参数一致）。 */
    async runPrediction(horizonHours = [1, 6, 24]) {
      this.taskError = ''
      this.notice = ''
      try {
        const data = await startMlTask({ taskType: 'PREDICT', horizonHours })
        this.task = {
          taskNo: data.taskNo || '',
          taskType: data.taskType || 'PREDICT',
          status: data.status || 'PENDING',
          modelVersion: ''
        }
        this.notice = `预测任务已提交（${data.taskNo || '无编号'}），正在生成结果`
        if (this.task.taskNo) this.startPolling()
        return true
      } catch (error) {
        this.taskError = error?.userMessage || '预测任务提交失败'
        return false
      }
    },

    /** §9.3 查询一次任务状态。 */
    async pollTask() {
      if (!this.task?.taskNo) return null
      const data = await fetchMlTask(this.task.taskNo)
      this.task = {
        ...this.task,
        status: data.status || this.task.status,
        taskType: data.taskType || this.task.taskType,
        modelVersion: typeof data.modelVersion === 'string' ? data.modelVersion : ''
      }
      if (this.taskFinished) {
        this.stopPolling()
        if (this.task.status === 'SUCCEEDED') {
          this.notice = '预测已完成，正在刷新结果'
          await this.load()
        } else {
          this.taskError = '预测任务未成功，请检查 ML 服务后重试'
        }
      }
      return this.task
    },

    startPolling() {
      this.stopPolling()
      this.taskPolling = true
      this.taskAttempts = 0
      const tick = async () => {
        if (!this.taskPolling) return
        this.taskAttempts += 1
        try {
          await this.pollTask()
        } catch (error) {
          this.taskError = error?.userMessage || '任务状态查询失败'
        }
        if (!this.taskPolling || this.taskFinished) return
        if (this.taskAttempts >= ML_POLL_MAX_ATTEMPTS) {
          this.stopPolling()
          this.taskError = `任务 ${this.task?.taskNo} 仍在运行，请稍后重新查询`
          return
        }
        pollTimer = setTimeout(tick, ML_POLL_INTERVAL_MS)
      }
      pollTimer = setTimeout(tick, ML_POLL_INTERVAL_MS)
    },

    stopPolling() {
      if (pollTimer) clearTimeout(pollTimer)
      pollTimer = null
      this.taskPolling = false
    },

    clearMessages() {
      this.error = ''
      this.notice = ''
      this.taskError = ''
    },

    /** 预测小时数 → 目标时间的展示用标签。 */
    horizonLabel(horizonHour) {
      const value = toInteger(horizonHour)
      return value === null ? '—' : `未来 ${value} 小时`
    }
  }
})
