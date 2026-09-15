<script setup>
/**
 * 充电桩管理（接口文档 §7.6–§7.10）。
 *
 * 对齐 Qt 管理端并补齐批量创建：
 * 设备筛选、状态变更（必填原因 + 版本校验）、远程重启（二次确认 + 命令编号轮询）、
 * 批量创建（一次最多 100 台，全部成功或全部回滚）。
 */
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import ConfirmDialog from '@/components/ConfirmDialog.vue'
import DataTable from '@/components/DataTable.vue'
import FilterBar from '@/components/FilterBar.vue'
import FormDialog from '@/components/FormDialog.vue'
import StatusPill from '@/components/StatusPill.vue'
import { CHARGER_SETTABLE_STATUS, CHARGER_STATUS, CHARGER_TYPES } from '@/utils/domain'
import { useChargersStore } from '@/stores/chargers'
import { formatDateTime, formatMinutes, formatPower, formatInt } from '@/utils/format'

const chargers = useChargersStore()

const stationId = ref(null)
const statusFilter = ref(null)
const typeFilter = ref(null)
const keyword = ref('')

const batchOpen = ref(false)
const statusTarget = ref(null)
const restartTarget = ref(null)

onMounted(async () => {
  await Promise.all([chargers.load(), chargers.loadStationOptions()])
})

onBeforeUnmount(() => {
  chargers.stopPolling()
})

const columns = [
  { key: 'code', label: '电桩编号' },
  { key: 'stationId', label: '所属站点', format: (value, row) => stationLabel(value, row) },
  { key: 'chargerType', label: '类型', format: value => chargerTypeLabel(value) },
  { key: 'powerWatt', label: '功率', align: 'right', format: value => formatPower(value) },
  { key: 'statusText', label: '状态' },
  { key: 'totalCount', label: '充电次数', align: 'right', format: value => formatInt(value) },
  { key: 'totalMinutes', label: '累计时长', align: 'right', format: value => formatMinutes(value) },
  { key: 'version', label: '版本', align: 'right' }
]

const statusOptions = CHARGER_STATUS.map(item => ({ value: item.value, label: item.label }))

const statusFields = computed(() => [
  {
    key: 'targetStatus',
    label: '目标状态',
    type: 'select',
    options: statusOptions.filter(item => CHARGER_SETTABLE_STATUS.includes(item.value))
  }
])

const batchFields = computed(() => [
  {
    key: 'stationId',
    label: '所属站点',
    type: 'select',
    options: chargers.stationOptions.map(item => ({ value: item.id, label: `${item.name}（${item.code}）` }))
  },
  { key: 'codePrefix', label: '编号前缀', required: true, maxLength: 24, placeholder: '如 ZGC-DC' },
  { key: 'startIndex', label: '起始序号', type: 'number', required: true, min: 0, max: 99, default: 11 },
  { key: 'count', label: '创建数量', type: 'number', required: true, min: 1, max: 100, default: 1 },
  {
    key: 'chargerType',
    label: '电桩类型',
    type: 'select',
    options: CHARGER_TYPES.map(item => ({ value: item.value, label: item.label }))
  },
  { key: 'powerWatt', label: '单桩功率（W）', type: 'number', required: true, min: 1, max: 1000000, default: 120000 },
  { key: 'connectorStandard', label: '接口标准', default: 'GB/T 20234.3', maxLength: 32 }
])

const hasOptions = computed(() => chargers.stationOptions.length > 0)

function stationLabel(stationIdValue, row) {
  const found = chargers.stationOptions.find(item => item.id === stationIdValue)
  if (found) return `${found.name}（${found.code}）`
  if (row?.stationCode) return row.stationCode
  return `站点 #${stationIdValue ?? '—'}`
}

function chargerTypeLabel(value) {
  const found = CHARGER_TYPES.find(item => item.value === value)
  return found ? found.label : '—'
}

function statusTone(text) {
  const found = CHARGER_STATUS.find(item => item.label === text)
  return found ? found.tone : 'muted'
}

function applyFilters() {
  chargers.setFilter({
    stationId: stationId.value,
    status: statusFilter.value,
    chargerType: typeFilter.value,
    keyword: keyword.value
  })
}

function resetFilters() {
  stationId.value = null
  statusFilter.value = null
  typeFilter.value = null
  keyword.value = ''
  chargers.resetFilters()
}

/** 批量编号：与后端 `站点编码-DC/AC-两位序号` 规则一致，前端只做序号补齐。 */
function buildCodes(prefix, startIndex, count) {
  const list = []
  for (let offset = 0; offset < count; offset += 1) {
    list.push(`${prefix}-${String(startIndex + offset).padStart(2, '0')}`)
  }
  return list
}

async function submitBatch(values) {
  const codes = buildCodes(values.codePrefix, values.startIndex, values.count)
  const ok = await chargers.batchCreate({
    stationId: values.stationId,
    chargers: codes.map(code => ({
      code,
      chargerType: values.chargerType,
      powerWatt: values.powerWatt,
      connectorStandard: values.connectorStandard || 'GB/T 20234.3'
    }))
  })
  if (ok) batchOpen.value = false
}

async function submitStatus({ reason, targetStatus }) {
  if (!statusTarget.value) return
  const ok = await chargers.setStatus(statusTarget.value, targetStatus, reason)
  if (ok) statusTarget.value = null
}

async function submitRestart({ reason }) {
  if (!restartTarget.value) return
  const ok = await chargers.restart(restartTarget.value, reason)
  if (ok) restartTarget.value = null
}
</script>

<template>
  <div class="view">
    <p v-if="chargers.notice" class="alert alert--ok" data-testid="chargers-notice">{{ chargers.notice }}</p>
    <p v-if="chargers.error" class="alert alert--error" data-testid="chargers-error-banner">
      {{ chargers.error }}
    </p>
    <p v-if="chargers.conflict" class="alert" data-testid="chargers-conflict">{{ chargers.conflict }}</p>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>设备筛选</h2>
          <p class="panel__hint">使用中的设备由服务端保护，必须先受控结束并结算才能变更状态</p>
        </div>
        <button
          type="button"
          class="btn btn--sm btn--primary"
          :disabled="!hasOptions"
          data-testid="charger-batch"
          @click="batchOpen = true"
        >
          批量创建
        </button>
      </div>

      <FilterBar test-id="chargers-filter" :busy="chargers.loading" @submit="applyFilters">
        <label class="field">
          <span>所属站点</span>
          <select v-model="stationId" data-testid="chargers-station">
            <option :value="null">全部站点</option>
            <option v-for="item in chargers.stationOptions" :key="item.id" :value="item.id">{{ item.name }}</option>
          </select>
        </label>
        <label class="field">
          <span>状态</span>
          <select v-model="statusFilter" data-testid="chargers-status">
            <option :value="null">全部状态</option>
            <option v-for="item in CHARGER_STATUS" :key="item.value" :value="item.value">{{ item.label }}</option>
          </select>
        </label>
        <label class="field">
          <span>类型</span>
          <select v-model="typeFilter" data-testid="chargers-type">
            <option :value="null">全部类型</option>
            <option v-for="item in CHARGER_TYPES" :key="item.value" :value="item.value">{{ item.label }}</option>
          </select>
        </label>
        <label class="field">
          <span>编号关键词</span>
          <input v-model="keyword" type="search" placeholder="按电桩编号搜索" data-testid="chargers-keyword" />
        </label>
        <template #actions>
          <button type="button" class="btn" data-testid="chargers-reset" @click="resetFilters">重置</button>
        </template>
      </FilterBar>
    </section>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>设备列表</h2>
          <p class="panel__hint">共 {{ chargers.total }} 台设备</p>
        </div>
      </div>

      <DataTable
        test-id="chargers"
        :columns="columns"
        :rows="chargers.items"
        row-key="id"
        :loading="chargers.loading"
        :error="chargers.error"
        empty-text="没有匹配的设备，可调整筛选条件或批量创建"
        :selected-key="chargers.selectedChargerId"
        :row-test-id="row => `charger-row-${row.id}`"
        @row-click="row => chargers.select(row.id)"
        @retry="chargers.load()"
      >
        <template #cell-statusText="{ row }">
          <StatusPill :text="row.statusText" :tone="statusTone(row.statusText)" :test-id="`charger-state-${row.id}`" />
        </template>
        <template #cell-version="{ row }">
          <span class="row-actions">
            <button
              type="button"
              class="btn btn--sm"
              :data-testid="`charger-status-${row.id}`"
              @click.stop="statusTarget = row"
            >
              修改状态
            </button>
            <button
              type="button"
              class="btn btn--sm btn--ghost"
              :data-testid="`charger-restart-${row.id}`"
              @click.stop="restartTarget = row"
            >
              远程重启
            </button>
          </span>
        </template>
      </DataTable>

      <div class="panel__row panel__row--end pager">
        <button
          type="button"
          class="btn btn--sm"
          :disabled="chargers.page <= 1"
          data-testid="chargers-prev"
          @click="chargers.goToPage(chargers.page - 1)"
        >
          上一页
        </button>
        <span class="muted">第 {{ chargers.page }} / {{ chargers.pageCount }} 页</span>
        <button
          type="button"
          class="btn btn--sm"
          :disabled="chargers.page >= chargers.pageCount"
          data-testid="chargers-next"
          @click="chargers.goToPage(chargers.page + 1)"
        >
          下一页
        </button>
      </div>
    </section>

    <section v-if="chargers.command" class="panel" v-reveal data-testid="charger-command-panel">
      <div class="panel__title">
        <div>
          <h2>最近一次远程重启</h2>
          <p class="panel__hint">
            命令编号 {{ chargers.command.commandNo }} · 提交于 {{ formatDateTime(chargers.command.createdAt) }}
          </p>
        </div>
        <div class="panel__row">
          <StatusPill :text="chargers.commandLabel" :tone="chargers.commandTone" test-id="charger-command-status" />
          <button
            type="button"
            class="btn btn--sm"
            :disabled="chargers.commandPolling"
            data-testid="charger-command-refresh"
            @click="chargers.pollCommand()"
          >
            {{ chargers.commandPolling ? '轮询中…' : '立即查询' }}
          </button>
        </div>
      </div>
      <ul class="metric-list">
        <li>
          <span>设备</span>
          <strong>{{ chargers.command.chargerCode || '—' }}</strong>
        </li>
        <li>
          <span>完成时间</span>
          <strong>{{ chargers.command.completedAt ? formatDateTime(chargers.command.completedAt) : '待执行' }}</strong>
        </li>
        <li v-if="chargers.command.errorSummary">
          <span>失败原因</span>
          <strong>{{ chargers.command.errorSummary }}</strong>
        </li>
      </ul>
    </section>

    <FormDialog
      v-if="batchOpen"
      test-id="charger-batch-dialog"
      title="批量创建设备"
      hint="一次最多 100 台，全部成功或全部回滚；编号由前缀与两位序号自动生成"
      :fields="batchFields"
      submit-label="批量创建"
      :loading="chargers.saving"
      :error="chargers.error"
      @submit="submitBatch"
      @cancel="batchOpen = false"
    />

    <ConfirmDialog
      v-if="statusTarget"
      :title="`修改设备状态 ${statusTarget.code}`"
      hint="活动设备不得直接变为空闲、故障或停用；提交时携带当前 version"
      :fields="statusFields"
      confirm-label="提交状态变更"
      :loading="chargers.saving"
      :error="chargers.error"
      @confirm="submitStatus"
      @cancel="statusTarget = null"
    />

    <ConfirmDialog
      v-if="restartTarget"
      :title="`远程重启 ${restartTarget.code}`"
      :hint="`重启会立刻中断该设备的服务（进入重启中），充电中的订单必须先受控结算。`"
      danger
      confirm-label="确认重启"
      :loading="chargers.saving"
      :error="chargers.error"
      @confirm="submitRestart"
      @cancel="restartTarget = null"
    />
  </div>
</template>

<style scoped>
.row-actions {
  display: inline-flex;
  gap: var(--ncs-s-2);
}

.pager {
  justify-content: flex-end;
  margin-top: var(--ncs-s-3);
}
</style>
