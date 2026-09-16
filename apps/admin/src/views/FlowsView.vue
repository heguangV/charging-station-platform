<script setup>
/**
 * 活动流程（接口文档 §8.1、§8.2）。
 *
 * 强制释放只对状态 20/30 开放：必须二次确认、填写原因、选择目标设备状态并提交当前流程版本。
 * 该操作属于敏感操作，服务端要求 15 分钟内重新验证，由 auth store 统一处理。
 */
import { computed, onMounted, ref } from 'vue'
import ConfirmDialog from '@/components/ConfirmDialog.vue'
import DataTable from '@/components/DataTable.vue'
import FilterBar from '@/components/FilterBar.vue'
import StatusPill from '@/components/StatusPill.vue'
import { CHARGER_SETTABLE_STATUS, CHARGER_STATUS, FLOW_RELEASABLE_STATUS, FLOW_STATUS, toneOfStatusText } from '@/utils/domain'
import { useFlowsStore } from '@/stores/flows'
import { formatDateTime } from '@/utils/format'

const flows = useFlowsStore()

const statusFilter = ref(null)
const orderNo = ref('')
const releaseTarget = ref(null)

onMounted(() => {
  flows.load()
})

const columns = [
  { key: 'flowNo', label: '流程编号' },
  { key: 'userId', label: '用户 ID', align: 'right' },
  { key: 'stationId', label: '站点 ID', align: 'right' },
  { key: 'chargerCode', label: '电桩编号' },
  { key: 'statusText', label: '状态' },
  { key: 'createdAt', label: '创建时间', format: value => formatDateTime(value) }
]

const releaseFields = computed(() => [
  {
    key: 'nextChargerStatus',
    label: '释放后设备状态',
    type: 'select',
    options: CHARGER_STATUS.filter(item => CHARGER_SETTABLE_STATUS.includes(item.value)).map(item => ({
      value: item.value,
      label: item.label
    }))
  }
])

const releaseHint = computed(() => {
  if (!releaseTarget.value) return ''
  return `确认强制释放流程 ${releaseTarget.value.flowNo}？提交的流程版本为 ${releaseTarget.value.version}，落后会返回 VERSION_CONFLICT。`
})

function applyFilters() {
  flows.setFilter({
    status: statusFilter.value,
    orderNo: orderNo.value
  })
}

function resetFilters() {
  statusFilter.value = null
  orderNo.value = ''
  flows.resetFilters()
}

async function submitRelease({ reason, nextChargerStatus }) {
  if (!releaseTarget.value) return
  const ok = await flows.forceRelease(releaseTarget.value, { reason, nextChargerStatus })
  if (ok) releaseTarget.value = null
}
</script>

<template>
  <div class="view">
    <p v-if="flows.notice" class="alert alert--ok" data-testid="flows-notice">{{ flows.notice }}</p>
    <p v-if="flows.error" class="alert alert--error" data-testid="flows-error-banner">{{ flows.error }}</p>
    <p v-if="flows.conflict" class="alert" data-testid="flows-conflict">
      {{ flows.conflict }}
      <span class="alert__actions">
        <button type="button" class="btn btn--sm" @click="flows.load()">刷新列表</button>
      </span>
    </p>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>流程筛选</h2>
          <p class="panel__hint">
            未完成/待恢复状态为 10/20/30/40/50/80，终态为 60/70/90；当前页可强制释放 {{ flows.releasableCount }} 条
          </p>
        </div>
      </div>

      <FilterBar test-id="flows-filter" :busy="flows.loading" @submit="applyFilters">
        <label class="field">
          <span>流程状态</span>
          <select v-model="statusFilter" data-testid="flows-status">
            <option :value="null">全部状态</option>
            <option v-for="item in FLOW_STATUS" :key="item.value" :value="item.value">
              {{ item.label }}（{{ item.value }}）
            </option>
          </select>
        </label>
        <label class="field">
          <span>站点 ID</span>
          <input v-model="orderNo" type="search" placeholder="按订单号筛选" data-testid="flows-order" />
        </label>
        <label class="field">
          <span>电桩 ID</span>

        </label>
        <label class="field">
          <span>用户 ID</span>

        </label>
        <template #actions>
          <button type="button" class="btn" data-testid="flows-reset" @click="resetFilters">重置</button>
        </template>
      </FilterBar>
    </section>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>流程列表</h2>
          <p class="panel__hint">共 {{ flows.total }} 条记录 · 强制释放仅对已预约/待启动状态开放</p>
        </div>
      </div>

      <DataTable
        test-id="flows"
        :columns="columns"
        :rows="flows.items"
        row-key="flowNo"
        :loading="flows.loading"
        :error="flows.error"
        empty-text="没有匹配的活动流程"
        :selected-key="flows.selectedFlowNo"
        :row-test-id="row => `flow-row-${row.flowNo}`"
        @row-click="row => flows.select(row.flowNo)"
        @retry="flows.load()"
      >
        <template #cell-statusText="{ row }">
          <StatusPill :text="row.statusText || '—'" :tone="toneOfStatusText(row.statusText)" :test-id="`flow-state-${row.flowNo}`" />
        </template>
        <template #cell-chargerCode="{ row }">
          {{ row.chargerCode || (row.chargerId ? `设备 #${row.chargerId}` : '未分配') }}
        </template>
        <template #cell-version="{ row }">
          <button
            type="button"
            class="btn btn--sm"
            :class="FLOW_RELEASABLE_STATUS.includes(row.status) ? 'btn--danger' : ''"
            :disabled="!FLOW_RELEASABLE_STATUS.includes(row.status)"
            :title="
              FLOW_RELEASABLE_STATUS.includes(row.status)
                ? '强制释放该流程'
                : '仅已预约(20)与待启动(30)可强制释放，充电中请使用设备重启的受控结算流程'
            "
            :data-testid="`flow-force-release-${row.flowNo}`"
            @click.stop="releaseTarget = row"
          >
            强制释放
          </button>
        </template>
      </DataTable>

      <div class="panel__row panel__row--end pager">
        <button
          type="button"
          class="btn btn--sm"
          :disabled="flows.page <= 1"
          data-testid="flows-prev"
          @click="flows.goToPage(flows.page - 1)"
        >
          上一页
        </button>
        <span class="muted">第 {{ flows.page }} / {{ flows.pageCount }} 页</span>
        <button
          type="button"
          class="btn btn--sm"
          :disabled="flows.page >= flows.pageCount"
          data-testid="flows-next"
          @click="flows.goToPage(flows.page + 1)"
        >
          下一页
        </button>
      </div>
    </section>

    <ConfirmDialog
      v-if="releaseTarget"
      :title="`强制释放流程 ${releaseTarget.flowNo}`"
      :hint="releaseHint"
      danger
      confirm-label="确认强制释放"
      :fields="releaseFields"
      :loading="flows.saving"
      :error="flows.error"
      @confirm="submitRelease"
      @cancel="releaseTarget = null"
    />
  </div>
</template>

<style scoped>
.pager {
  justify-content: flex-end;
  margin-top: var(--ncs-s-3);
}
</style>
