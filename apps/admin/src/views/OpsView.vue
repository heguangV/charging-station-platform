<script setup>
/**
 * 运维页（Go 契约）：审计日志。
 *
 * Go 契约的审计对管理员可读（SUPER_ADMIN/OPERATOR/AUDITOR），仅支持
 * actorId/action/resourceType/resourceId 过滤 + 分页（无时间范围）。
 * 一致性备份域在 Go 后端暂缺，页面显式提示未开放，不提供伪装入口。
 */
import { computed, onMounted, ref } from 'vue'
import DataTable from '@/components/DataTable.vue'
import FilterBar from '@/components/FilterBar.vue'
import { usePagination } from '@/composables/usePagination'
import { useAuthStore } from '@/stores/auth'
import { useOpsStore } from '@/stores/ops'
import { formatDateTime } from '@/utils/format'

const ops = useOpsStore()
const auth = useAuthStore()
const pager = usePagination({ pageSize: 20 })

const actorId = ref('')
const action = ref('')
const targetType = ref('')
const targetId = ref('')

onMounted(async () => {
  if (!auth.isLoggedIn) return
  await ops.loadAuditLogs()
  pager.observeRows(ops.audit.items)
})

const auditColumns = [
  { key: 'createdAt', label: '时间', format: value => formatDateTime(value) },
  { key: 'actorId', label: '操作者' },
  { key: 'action', label: '动作' },
  { key: 'targetType', label: '目标类型' },
  { key: 'targetId', label: '目标 ID' }
]

const auditSummary = computed(() => pager.rangeLabel.value)

function applyFilters() {
  ops.setAuditFilter({
    actorId: actorId.value,
    action: action.value,
    targetType: targetType.value,
    targetId: targetId.value
  })
}

function resetFilters() {
  actorId.value = ''
  action.value = ''
  targetType.value = ''
  targetId.value = ''
  ops.resetAuditFilters()
}

async function goAuditPage(page) {
  await ops.goToAuditPage(page)
  pager.sync(ops.audit)
  pager.observeRows(ops.audit.items)
}
</script>

<template>
  <div class="view">
    <p v-if="ops.notice" class="alert alert--ok" data-testid="ops-notice">{{ ops.notice }}</p>
    <p v-if="ops.error" class="alert alert--error" data-testid="ops-error-banner">{{ ops.error }}</p>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>审计日志</h2>
          <p class="panel__hint">只读：接口不提供修改与删除；Go 契约暂不支持时间范围过滤</p>
        </div>
        <span class="muted" data-testid="ops-audit-range">{{ auditSummary }}</span>
      </div>

      <FilterBar test-id="ops-filter" :busy="ops.auditLoading" @submit="applyFilters">
        <label class="field">
          <span>操作者</span>
          <input v-model="actorId" type="search" placeholder="管理员 ID" data-testid="ops-actor" />
        </label>
        <label class="field">
          <span>动作</span>
          <input v-model="action" type="search" placeholder="如 USER_FROZEN" data-testid="ops-action" />
        </label>
        <label class="field">
          <span>目标类型</span>
          <input v-model="targetType" type="search" placeholder="如 STATION / USER" data-testid="ops-target-type" />
        </label>
        <label class="field">
          <span>目标 ID</span>
          <input v-model="targetId" type="search" placeholder="目标主键" data-testid="ops-target-id" />
        </label>
        <template #actions>
          <button type="button" class="btn" data-testid="ops-reset" @click="resetFilters">重置</button>
        </template>
      </FilterBar>

      <DataTable
        test-id="ops"
        :columns="auditColumns"
        :rows="ops.audit.items"
        row-key="id"
        :loading="ops.auditLoading"
        :error="ops.auditError"
        empty-text="没有匹配的审计记录"
        :skeleton-rows="6"
        :row-test-id="row => `audit-row-${row.id}-${row.action}`"
        @retry="ops.loadAuditLogs()"
      />

      <div class="panel__row panel__row--end pager">
        <button
          type="button"
          class="btn btn--sm"
          :disabled="ops.audit.page <= 1"
          data-testid="ops-prev"
          @click="goAuditPage(ops.audit.page - 1)"
        >
          上一页
        </button>
        <span class="muted">第 {{ ops.audit.page }} 页</span>
        <button type="button" class="btn btn--sm" data-testid="ops-next" @click="goAuditPage(ops.audit.page + 1)">
          下一页
        </button>
      </div>
    </section>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>一致性备份</h2>
          <p class="panel__hint">备份能力依赖部署线（B-06）的数据库迁移与运维闭环</p>
        </div>
      </div>
      <p class="muted" data-testid="ops-backups-unavailable">
        备份管理暂未开放：Go 后端契约尚未包含备份与隔离恢复验证（待 B-01 确认后接入）。
      </p>
    </section>
  </div>
</template>

<style scoped>
.pager {
  justify-content: flex-end;
  margin-top: var(--ncs-s-3);
}
</style>
