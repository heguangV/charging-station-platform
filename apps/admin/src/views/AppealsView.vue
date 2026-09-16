<script setup>
/**
 * 申诉管理（UC-U-09，A-04 第 8/9 步）：申诉队列与审核。
 *
 * 审核通过（二次确认）后：申诉转 APPROVED、订单取消、实付金额退回用户钱包
 * （REFUND 流水），审计由服务端在同一事务写入。Go 契约下重复审核是幂等无操作，
 * 不会重复退款。
 */
import { onMounted, ref } from 'vue'
import ConfirmDialog from '@/components/ConfirmDialog.vue'
import DataTable from '@/components/DataTable.vue'
import FilterBar from '@/components/FilterBar.vue'
import StatusPill from '@/components/StatusPill.vue'
import { useAppealsStore } from '@/stores/appeals'
import { formatAmount, formatDateTime } from '@/utils/format'

const appeals = useAppealsStore()

const statusFilter = ref(null)
const approveTarget = ref(null)
const rejectTarget = ref(null)

onMounted(() => {
  appeals.load()
})

const statusOptions = [
  { label: '全部', value: null },
  { label: '待处理', value: 'PENDING' },
  { label: '已通过', value: 'APPROVED' },
  { label: '已驳回', value: 'REJECTED' }
]

const columns = [
  { key: 'id', label: '申诉编号', align: 'right' },
  { key: 'orderNo', label: '订单号' },
  { key: 'reason', label: '申诉原因' },
  // 金额口径与其它管理端表格一致：契约给整数分，展示按元（旧表头写"分"、
  // 单元格却渲染"1.70 元"，同一行两种单位）。
  { key: 'orderAmountCent', label: '应付（元）', align: 'right', format: value => formatAmount(value) },
  { key: 'orderPaidCent', label: '实付（元）', align: 'right', format: value => formatAmount(value) },
  { key: 'createdAt', label: '提交时间', format: value => formatDateTime(value) },
  { key: 'statusText', label: '状态' },
  { key: 'decisionReason', label: '处理意见' },
  // 操作有自己的列：以前按钮借「提交时间」列渲染，时间就永远不显示了。
  { key: 'actions', label: '操作', align: 'right' }
]

function statusTone(status) {
  if (status === 'PENDING') return 'warn'
  if (status === 'APPROVED') return 'ok'
  if (status === 'REJECTED') return 'muted'
  return 'muted'
}

function statusText(status) {
  if (status === 'PENDING') return '待处理'
  if (status === 'APPROVED') return '已通过'
  if (status === 'REJECTED') return '已驳回'
  return status || '—'
}

function applyFilters() {
  appeals.setFilter({ status: statusFilter.value })
}

function resetFilters() {
  statusFilter.value = null
  appeals.resetFilters()
}

async function confirmApprove() {
  if (!approveTarget.value) return
  const ok = await appeals.approve(approveTarget.value)
  if (ok) approveTarget.value = null
}

/** 驳回必须写处理意见：这句话会给用户看，也是审计里唯一说明"为什么不退"的记录。 */
async function confirmReject({ reason }) {
  if (!rejectTarget.value) return
  const ok = await appeals.reject(rejectTarget.value, reason)
  if (ok) rejectTarget.value = null
}
</script>

<template>
  <div class="view" data-testid="appeals-view">
    <p v-if="appeals.notice" class="alert alert--ok" data-testid="appeals-notice">{{ appeals.notice }}</p>
    <p v-if="appeals.error" class="alert alert--error" data-testid="appeals-error">{{ appeals.error }}</p>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>申诉队列</h2>
          <p class="panel__hint">
            待处理 {{ appeals.pendingCount }} 条 · 通过＝订单取消并原路退款；驳回＝仅记录结论，订单与钱包不动。
            重复决策是幂等无操作
          </p>
        </div>
      </div>

      <FilterBar test-id="appeals-filter" :busy="appeals.loading" @submit="applyFilters">
        <label class="field">
          <span>状态</span>
          <select v-model="statusFilter" data-testid="appeals-status">
            <option v-for="item in statusOptions" :key="String(item.value)" :value="item.value">{{ item.label }}</option>
          </select>
        </label>
        <template #actions>
          <button type="button" class="btn" data-testid="appeals-reset" @click="resetFilters">重置</button>
        </template>
      </FilterBar>

      <DataTable
        test-id="appeals"
        :columns="columns"
        :rows="appeals.items"
        row-key="id"
        :loading="appeals.loading"
        :error="appeals.error"
        empty-text="没有匹配的申诉记录"
        :skeleton-rows="6"
        :row-test-id="row => `appeal-row-${row.id}`"
        @retry="appeals.load()"
      >
        <template #cell-statusText="{ row }">
          <StatusPill :text="statusText(row.status)" :tone="statusTone(row.status)" :test-id="`appeal-state-${row.id}`" />
        </template>
        <template #cell-actions="{ row }">
          <span class="row-actions">
            <button
              v-if="row.status === 'PENDING'"
              type="button"
              class="btn btn--sm btn--primary"
              :disabled="appeals.saving"
              :data-testid="`appeal-approve-${row.id}`"
              @click.stop="approveTarget = row"
            >
              审核通过
            </button>
            <button
              v-if="row.status === 'PENDING'"
              type="button"
              class="btn btn--sm"
              :disabled="appeals.saving"
              :data-testid="`appeal-reject-${row.id}`"
              @click.stop="rejectTarget = row"
            >
              驳回
            </button>
          </span>
        </template>
      </DataTable>

      <div class="panel__row panel__row--end pager">
        <button
          type="button"
          class="btn btn--sm"
          :disabled="appeals.page <= 1"
          data-testid="appeals-prev"
          @click="appeals.goToPage(appeals.page - 1)"
        >
          上一页
        </button>
        <span class="muted">第 {{ appeals.page }} / {{ appeals.pageCount }} 页</span>
        <button type="button" class="btn btn--sm" data-testid="appeals-next" @click="appeals.goToPage(appeals.page + 1)">
          下一页
        </button>
      </div>
    </section>

    <ConfirmDialog
      v-if="approveTarget"
      :title="`审核通过申诉 #${approveTarget.id}`"
      hint="确认后：申诉转已通过、订单取消、实付金额退回用户钱包，审计记录由服务端在同一事务写入。重复审核是幂等无操作。"
      :require-reason="false"
      confirm-label="确认通过"
      :loading="appeals.saving"
      :error="appeals.error"
      @confirm="confirmApprove"
      @cancel="approveTarget = null"
    />

    <ConfirmDialog
      v-if="rejectTarget"
      :title="`驳回申诉 #${rejectTarget.id}`"
      hint="驳回后：申诉转已驳回并记录处理意见；订单保持已完成、钱包不动（退款是审核通过才做的事）。重复决策不会改变结论。"
      confirm-label="确认驳回"
      :loading="appeals.saving"
      :error="appeals.error"
      @confirm="confirmReject"
      @cancel="rejectTarget = null"
    />
  </div>
</template>

<style scoped>
.pager {
  justify-content: flex-end;
  margin-top: var(--ncs-s-3);
}
</style>
