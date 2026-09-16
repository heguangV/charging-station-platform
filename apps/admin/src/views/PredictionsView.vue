<script setup>
/**
 * 智能预测（接口文档 §9.1–§9.3）。
 *
 * 触发入口是 §9.2 的 POST /admin/ml-tasks（taskType=PREDICT，周期固定 1/6/24）。
 * 接口文档与现有服务端都没有“任务列表”接口（只有 POST /admin/ml-tasks 与
 * GET /admin/ml-tasks/{taskNo}），因此这里只轮询本次启动的任务编号，不臆造列表接口。
 */
import { onBeforeUnmount, onMounted, ref } from 'vue'
import DataTable from '@/components/DataTable.vue'
import FilterBar from '@/components/FilterBar.vue'
import StatusPill from '@/components/StatusPill.vue'
import { PREDICTION_HORIZONS } from '@/utils/domain'
import { usePredictionsStore } from '@/stores/predictions'
import { formatDateTime, formatEnergy, formatInt } from '@/utils/format'

const predictions = usePredictionsStore()

const stationId = ref(null)
const horizonHour = ref(null)

onMounted(async () => {
  await Promise.all([predictions.load(), predictions.loadStationOptions()])
})

onBeforeUnmount(() => {
  predictions.stopPolling()
})

const columns = [
  { key: 'targetAt', label: '目标时段', format: value => formatDateTime(value) },
  { key: 'stationId', label: '站点' },
  { key: 'horizonHour', label: '预测周期', align: 'right', format: value => predictions.horizonLabel(value) },
  {
    key: 'predictedEnergyMwh',
    label: '预测电量（kWh）',
    align: 'right',
    format: value => formatEnergy(value)
  },
  { key: 'predictedIdleCount', label: '预计空闲桩', align: 'right', format: value => formatInt(value) },
  { key: 'peakFlag', label: '负荷提示', format: value => (value === true ? '高峰时段' : '常规') },
  { key: 'staleFlag', label: '数据状态', format: value => (value === true ? '已过期' : '有效') },
  { key: 'modelVersion', label: '模型版本' },
  { key: 'generatedAt', label: '生成时间', format: value => formatDateTime(value) }
]

/**
 * 站点名称解析：预测响应只带 stationId，这里用已加载的站点下拉选项做展示映射，
 * 找不到时回落到“站点 #id”，不改变任何数据。
 */
function stationName(value) {
  const found = predictions.stationOptions.find(item => item.id === value)
  return found ? found.name : `站点 #${value ?? '—'}`
}

function applyFilters() {
  predictions.setFilter({ stationId: stationId.value, horizonHour: horizonHour.value })
}

function resetFilters() {
  stationId.value = null
  horizonHour.value = null
  predictions.setFilter({ stationId: null, horizonHour: null })
}
</script>

<template>
  <div class="view">
    <p v-if="predictions.notice" class="alert alert--ok" data-testid="predictions-notice">
      {{ predictions.notice }}
    </p>
    <p v-if="predictions.taskError" class="alert alert--error" data-testid="predictions-task-error">
      {{ predictions.taskError }}
    </p>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>预测查询</h2>
          <p class="panel__hint">按站点与预测周期查询；过期数据仍会展示，但会标记为“已过期”</p>
        </div>
        <button
          type="button"
          class="btn btn--sm btn--primary"
          :disabled="predictions.taskPolling"
          data-testid="predictions-run"
          @click="predictions.runPrediction([1, 6, 24])"
        >
          {{ predictions.taskPolling ? '预测中…' : '运行预测' }}
        </button>
      </div>

      <FilterBar test-id="predictions-filter" :busy="predictions.loading" @submit="applyFilters">
        <label class="field">
          <span>站点</span>
          <select v-model="stationId" data-testid="predictions-station">
            <option :value="null">全部站点</option>
            <option v-for="item in predictions.stationOptions" :key="item.id" :value="item.id">{{ item.name }}</option>
          </select>
        </label>
        <label class="field">
          <span>预测周期</span>
          <select v-model="horizonHour" data-testid="predictions-horizon">
            <option :value="null">全部周期</option>
            <option v-for="item in PREDICTION_HORIZONS" :key="item.value" :value="item.value">{{ item.label }}</option>
          </select>
        </label>
        <template #actions>
          <button type="button" class="btn" data-testid="predictions-reset" @click="resetFilters">重置</button>
        </template>
      </FilterBar>
    </section>

    <section v-if="predictions.task" class="panel" v-reveal data-testid="predictions-task-panel">
      <div class="panel__title">
        <div>
          <h2>最近一次预测任务</h2>
          <p class="panel__hint">
            任务编号 {{ predictions.task.taskNo }} · 类型 {{ predictions.task.taskType }}
            <template v-if="predictions.task.modelVersion"> · 模型 {{ predictions.task.modelVersion }}</template>
          </p>
        </div>
        <div class="panel__row">
          <StatusPill :text="predictions.taskLabel" :tone="predictions.taskTone" test-id="predictions-task-status" />
          <button
            type="button"
            class="btn btn--sm"
            :disabled="predictions.taskPolling || predictions.taskFinished"
            data-testid="predictions-task-refresh"
            @click="predictions.pollTask()"
          >
            {{ predictions.taskPolling ? '轮询中…' : '立即查询' }}
          </button>
        </div>
      </div>
      <p class="panel__hint">
        服务端只提供“按任务编号查询”接口（GET /admin/ml-tasks/&#123;taskNo&#125;），没有历史任务列表接口，
        因此本页只追踪本次触发的任务。
      </p>
    </section>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>预测结果</h2>
          <p class="panel__hint">
            高峰时段 {{ predictions.peakCount }} 条 · 已过期 {{ predictions.staleCount }} 条 · 预测仅供运营参考
          </p>
        </div>
      </div>

      <DataTable
        test-id="predictions"
        :columns="columns"
        :rows="predictions.items"
        row-key="targetAt"
        :loading="predictions.loading"
        :error="predictions.error"
        empty-text="暂无预测结果，可点击“运行预测”生成"
        :row-test-id="row => `prediction-row-${row.stationId}-${row.horizonHour}-${row.targetAt}`"
        @retry="predictions.load()"
      >
        <template #cell-stationId="{ row }">
          {{ stationName(row.stationId) }}
        </template>
        <template #cell-peakFlag="{ row }">
          <StatusPill
            :text="row.peakFlag === true ? '高峰时段' : '常规'"
            :tone="row.peakFlag === true ? 'warn' : 'muted'"
            :test-id="`prediction-peak-${row.stationId}-${row.horizonHour}-${row.targetAt}`"
          />
        </template>
        <template #cell-staleFlag="{ row }">
          <StatusPill
            :text="row.staleFlag === true ? '已过期' : '有效'"
            :tone="row.staleFlag === true ? 'danger' : 'ok'"
            :test-id="`prediction-stale-${row.stationId}-${row.horizonHour}-${row.targetAt}`"
          />
        </template>
      </DataTable>
    </section>
  </div>
</template>
