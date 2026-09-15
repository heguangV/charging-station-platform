<script setup>
/**
 * 运维页（接口文档 §8.5–§8.8）：审计日志 + 一致性备份与隔离恢复验证。
 *
 * 两个接口都需要 OWNER 权限。审计日志接口只返回 items/page/pageSize（没有 total），
 * 因此分页用“本页是否满页”判断是否还有下一页；备份创建必须先二次确认，
 * 创建与验证都属于敏感操作，由 auth store 负责重新验证。
 */
import { computed, onMounted, ref } from 'vue'
import ConfirmDialog from '@/components/ConfirmDialog.vue'
import DataTable from '@/components/DataTable.vue'
import FilterBar from '@/components/FilterBar.vue'
import StatusPill from '@/components/StatusPill.vue'
import { usePagination } from '@/composables/usePagination'
import { useAuthStore } from '@/stores/auth'
import { useOpsStore } from '@/stores/ops'
import { formatBytes, formatDateTime, fromDateTimeInputValue } from '@/utils/format'

const ops = useOpsStore()
const auth = useAuthStore()
const pager = usePagination({ pageSize: 20 })

const actorId = ref('')
const action = ref('')
const targetType = ref('')
const targetId = ref('')
const fromAt = ref('')
const toAt = ref('')

const backupConfirmOpen = ref(false)

onMounted(async () => {
  if (!auth.isOwner) return
  await Promise.all([ops.loadAuditLogs(), ops.loadBackups()])
  pager.observeRows(ops.audit.items)
})

const auditColumns = [
  { key: 'at', label: '时间', format: value => formatDateTime(value) },
  { key: 'actorId', label: '操作者' },
  { key: 'action', label: '动作' },
  { key: 'targetType', label: '目标类型' },
  { key: 'targetId', label: '目标 ID' },
  { key: 'reason', label: '原因' }
]

const backupColumns = [
  { key: 'backupNo', label: '备份编号' },
  { key: 'status', label: '状态' },
  { key: 'checksum', label: '校验和', format: value => (typeof value === 'string' && value ? `${value.slice(0, 16)}…` : '—') },
  { key: 'sizeBytes', label: '大小', align: 'right', format: value => formatBytes(value) },
  { key: 'createdAt', label: '创建时间', format: value => formatDateTime(value) },
  { key: 'verificationStatus', label: '验证状态', format: value => value || '未验证' }
]

const auditSummary = computed(() => pager.rangeLabel.value)

function applyFilters() {
  ops.setAuditFilter({
    actorId: actorId.value,
    action: action.value,
    targetType: targetType.value,
    targetId: targetId.value,
    // 界面按本地时间输入，提交前换算为 UTC 秒（§1.4 时间约定）。
    fromAt: fromDateTimeInputValue(fromAt.value),
    toAt: fromDateTimeInputValue(toAt.value)
  })
}

function resetFilters() {
  actorId.value = ''
  action.value = ''
  targetType.value = ''
  targetId.value = ''
  fromAt.value = ''
  toAt.value = ''
  ops.resetAuditFilters()
}

async function goAuditPage(page) {
  await ops.goToAuditPage(page)
  pager.sync(ops.audit)
  pager.observeRows(ops.audit.items)
}

function backupTone(status) {
  if (status === 'SUCCEEDED' || status === 'READY') return 'ok'
  if (status === 'FAILED') return 'danger'
  return 'warn'
}

function verificationTone(status) {
  if (status === 'SUCCEEDED') return 'ok'
  if (status === 'FAILED') return 'danger'
  return 'muted'
}

async function confirmCreateBackup() {
  const ok = await ops.createBackup()
  if (ok) backupConfirmOpen.value = false
}
</script>

<template>
  <div class="view">
    <p v-if="!auth.isOwner" class="alert" data-testid="ops-permission-hint">
      审计日志与备份管理需要 OWNER 权限，服务端会拒绝越权请求。
    </p>
    <p v-if="ops.notice" class="alert alert--ok" data-testid="ops-notice">{{ ops.notice }}</p>
    <p v-if="ops.error" class="alert alert--error" data-testid="ops-error-banner">{{ ops.error }}</p>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>审计日志</h2>
          <p class="panel__hint">只读：接口不提供修改与删除；actorId 支持数字或 admin:数字</p>
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
          <input v-model="action" type="search" placeholder="如 ADMIN_DISABLED" data-testid="ops-action" />
        </label>
        <label class="field">
          <span>目标类型</span>
          <input v-model="targetType" type="search" placeholder="如 STATION / USER" data-testid="ops-target-type" />
        </label>
        <label class="field">
          <span>目标 ID</span>
          <input v-model="targetId" type="search" placeholder="目标主键" data-testid="ops-target-id" />
        </label>
        <label class="field">
          <span>起始时间（本地）</span>
          <input v-model="fromAt" type="datetime-local" data-testid="ops-from-at" />
        </label>
        <label class="field">
          <span>结束时间（本地）</span>
          <input v-model="toAt" type="datetime-local" data-testid="ops-to-at" />
        </label>
        <template #actions>
          <button type="button" class="btn" data-testid="ops-reset" @click="resetFilters">重置</button>
        </template>
      </FilterBar>

      <DataTable
        test-id="ops"
        :columns="auditColumns"
        :rows="ops.audit.items"
        row-key="at"
        :loading="ops.auditLoading"
        :error="ops.auditError"
        empty-text="没有匹配的审计记录"
        :skeleton-rows="6"
        :row-test-id="row => `audit-row-${row.at}-${row.action}`"
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
          <p class="panel__hint">
            共 {{ ops.backups.length }} 份备份 · 已验证 {{ ops.verifiedCount }} 份 ·
            接口不返回可由浏览器读取的真实文件路径
          </p>
        </div>
        <button type="button" class="btn btn--sm btn--primary" data-testid="backup-create" @click="backupConfirmOpen = true">
          创建备份
        </button>
      </div>

      <p v-if="ops.verification" class="alert alert--ok" data-testid="backup-verification">
        最近一次验证：{{ ops.verification.backupNo }} → {{ ops.verification.verificationStatus }}
      </p>

      <DataTable
        test-id="ops-backups"
        :columns="backupColumns"
        :rows="ops.backups"
        row-key="backupNo"
        :loading="ops.backupsLoading"
        :error="ops.backupsError"
        empty-text="暂无备份记录，可创建一份一致性备份"
        :row-test-id="row => `backup-row-${row.backupNo}`"
        @retry="ops.loadBackups()"
      >
        <template #cell-status="{ row }">
          <StatusPill :text="row.status || '—'" :tone="backupTone(row.status)" :test-id="`backup-state-${row.backupNo}`" />
        </template>
        <template #cell-verificationStatus="{ row }">
          <StatusPill
            :text="row.verificationStatus || '未验证'"
            :tone="verificationTone(row.verificationStatus)"
            :test-id="`backup-verification-state-${row.backupNo}`"
          />
        </template>
        <template #cell-createdAt="{ row }">
          <span class="row-actions">
            <button
              type="button"
              class="btn btn--sm btn--ghost"
              :disabled="ops.saving"
              :data-testid="`backup-verify-${row.backupNo}`"
              @click.stop="ops.verify(row.backupNo)"
            >
              隔离验证
            </button>
          </span>
        </template>
      </DataTable>
    </section>

    <ConfirmDialog
      v-if="backupConfirmOpen"
      title="创建一致性备份"
      hint="备份在服务端使用事务快照生成；创建后可用“隔离验证”在独立临时路径恢复校验，不会覆盖当前数据库。"
      :require-reason="false"
      confirm-label="确认创建"
      :loading="ops.saving"
      :error="ops.error"
      @confirm="confirmCreateBackup"
      @cancel="backupConfirmOpen = false"
    />
  </div>
</template>

<style scoped>
.pager {
  justify-content: flex-end;
  margin-top: var(--ncs-s-3);
}

.row-actions {
  display: inline-flex;
  gap: var(--ncs-s-2);
}
</style>
