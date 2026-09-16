<script setup>
/**
 * 用户管理（接口文档 §6.4–§6.7）。
 *
 * 手机号只支持完整 11 位精确匹配或后四位匹配（接口不提供模糊扫描）；
 * 冻结前先读取用户详情拿到最新 version，确认后提交原因——
 * 这与原 Qt 管理端的处理一致，也避免用过期版本触发 VERSION_CONFLICT。
 */
import { computed, onMounted, ref } from 'vue'
import AppSkeleton from '@/components/AppSkeleton.vue'
import ConfirmDialog from '@/components/ConfirmDialog.vue'
import DataTable from '@/components/DataTable.vue'
import FilterBar from '@/components/FilterBar.vue'
import FormDialog from '@/components/FormDialog.vue'
import StatusPill from '@/components/StatusPill.vue'
import { useUsersStore, USER_SORTS } from '@/stores/users'
import { USER_STATUS } from '@/utils/domain'
import { formatAmount, formatDateTime, formatEnergy, formatInt } from '@/utils/format'

const users = useUsersStore()

const keyword = ref('')
const statusFilter = ref(null)
const sort = ref('-registeredAt')

const statusTarget = ref(null)

const archiveOpen = ref(false)
const batchArchiveOpen = ref(false)
const archiveError = ref('')
const rosterError = ref('')

/** 建档手机号必须是可登录的手机号：与短信登录同一形状。 */
const ARCHIVE_PHONE_PATTERN = /^1[3-9]\d{9}$/

const archiveFields = [
  { key: 'phone', label: '手机号', required: true, maxLength: 11, placeholder: '11 位手机号，建档后走短信登录' },
  { key: 'displayName', label: '昵称（可选）', maxLength: 20, placeholder: '留空则默认“用户+手机号后四位”' }
]

const batchArchiveFields = [
  {
    key: 'roster',
    label: '名单（每行一条：手机号[,昵称]）',
    type: 'textarea',
    required: true,
    placeholder: '13912340000,老王\n13912340001\n13912340002,小李',
    rows: 8
  }
]

/**
 * 名单解析：一页 1..1000 条；手机号页内唯一。解析失败时给出可定位的
 * 行号，而不是让服务端的 400 代替表单校验。
 */
function parseRoster(raw) {
  const entries = []
  const seen = new Set()
  const lines = String(raw || '').split(/\r?\n/)
  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index].trim()
    if (!line) continue
    const [phone, ...rest] = line.split(/[,，]/)
    const displayName = rest.join(',').trim()
    const normalized = phone.trim()
    const where = `第 ${index + 1} 行`
    if (!ARCHIVE_PHONE_PATTERN.test(normalized)) {
      return { error: `${where}：${normalized || '（空）'} 不是可登录的手机号` }
    }
    if (displayName.length > 20) {
      return { error: `${where}：昵称超过 20 字` }
    }
    if (seen.has(normalized)) {
      return { error: `${where}：${normalized} 在名单中重复` }
    }
    seen.add(normalized)
    entries.push({ phone: normalized, displayName })
  }
  if (entries.length === 0) {
    return { error: '名单为空：每行一条“手机号[,昵称]”' }
  }
  if (entries.length > 1000) {
    return { error: `一次最多 1000 个账号（当前 ${entries.length} 条），更大数据量请使用种子脚本` }
  }
  return { entries }
}

async function submitArchive({ phone, displayName }) {
  if (!ARCHIVE_PHONE_PATTERN.test(phone.trim())) {
    archiveError.value = '手机号必须是 1 开头的 11 位数字（与短信登录一致）'
    return
  }
  const ok = await users.createUser({ phone: phone.trim(), displayName: displayName.trim() })
  if (ok) {
    archiveError.value = ''
    archiveOpen.value = false
  }
}

async function submitBatchArchive({ roster }) {
  const { entries, error } = parseRoster(roster)
  if (error) {
    rosterError.value = error
    return
  }
  const ok = await users.batchCreateUsers(entries)
  if (ok) {
    rosterError.value = ''
    batchArchiveOpen.value = false
  }
}

onMounted(() => {
  users.load()
})

const columns = [
  { key: 'id', label: '用户 ID', align: 'right' },
  { key: 'phoneMasked', label: '手机号（已脱敏）' },
  { key: 'nickname', label: '昵称' },
  { key: 'balanceCent', label: '钱包余额（元）', align: 'right', format: value => formatAmount(value) },
  { key: 'debtCent', label: '欠费（元）', align: 'right', format: value => formatAmount(value) },
  { key: 'registeredAt', label: '注册时间', format: value => formatDateTime(value) },
  { key: 'statusText', label: '状态' }
]

const orderColumns = [
  { key: 'orderNo', label: '订单号' },
  { key: 'stationName', label: '站点' },
  { key: 'chargerCode', label: '电桩编号' },
  { key: 'statusText', label: '状态' },
  { key: 'energyMwh', label: '电量（kWh）', align: 'right', format: value => formatEnergy(value) },
  { key: 'amountCent', label: '金额（元）', align: 'right', format: value => formatAmount(value) }
]

const statusHint = computed(() => {
  if (!statusTarget.value) return ''
  const action = statusTarget.value.status === 1 ? '冻结' : '解冻'
  const extra =
    statusTarget.value.status === 1
      ? '冻结会立即撤销该用户的登录会话并阻止新流程，进行中的充电继续由服务端计费并可正常结算。'
      : '解冻后用户可以正常登录并发起新的充电流程。'
  return `确认${action}用户 #${statusTarget.value.id}（${statusTarget.value.phoneMasked}）？${extra}`
})

function statusTone(text) {
  const found = USER_STATUS.find(item => item.label === text)
  return found ? found.tone : 'muted'
}

function applyFilters() {
  users.setFilter({
    keyword: keyword.value,
    status: statusFilter.value
  })
}

function resetFilters() {
  keyword.value = ''
  statusFilter.value = null
  users.resetFilters()
}

/** 冻结/解冻前必须先取详情：列表响应不含 version。 */
async function openStatusDialog(row) {
  const detail = await users.loadDetail(row.id)
  if (detail) statusTarget.value = detail
}

async function submitStatus({ reason }) {
  if (!statusTarget.value) return
  const target = statusTarget.value
  const nextStatus = target.status === 1 ? 0 : 1
  const ok = await users.setStatus(target, nextStatus, reason)
  if (ok) statusTarget.value = null
}

async function openOrders(row) {
  await Promise.all([users.loadDetail(row.id), users.loadOrders(row.id)])
}
</script>

<template>
  <div class="view">
    <p v-if="users.notice" class="alert alert--ok" data-testid="users-notice">{{ users.notice }}</p>
    <p v-if="users.error" class="alert alert--error" data-testid="users-error-banner">{{ users.error }}</p>
    <p v-if="users.conflict" class="alert" data-testid="users-conflict">{{ users.conflict }}</p>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>用户检索</h2>
          <p class="panel__hint">手机号只支持完整 11 位精确匹配或后四位匹配；管理员查询会写入审计日志</p>
        </div>
      </div>

      <FilterBar test-id="users-filter" :busy="users.loading" @submit="applyFilters">
        <label class="field">
          <span>完整手机号</span>
          <input v-model="keyword" type="search" placeholder="按昵称关键字搜索" data-testid="users-keyword" />
        </label>
        <label class="field">
          <span>手机号后四位</span>

        </label>
        <label class="field">
          <span>账号状态</span>
          <select v-model="statusFilter" data-testid="users-status">
            <option :value="null">全部状态</option>
            <option :value="1">正常</option>
            <option :value="0">冻结</option>
          </select>
        </label>
        <label class="field">
          <span>排序</span>
          <select v-model="sort" data-testid="users-sort">
            <option v-for="item in USER_SORTS" :key="item.value" :value="item.value">{{ item.label }}</option>
          </select>
        </label>
        <template #actions>
          <button type="button" class="btn" data-testid="users-reset" @click="resetFilters">重置</button>
        </template>
      </FilterBar>
    </section>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>用户列表</h2>
          <p class="panel__hint">共 {{ users.total }} 位用户 · 手机号已脱敏</p>
        </div>
        <span class="row-actions">
          <button type="button" class="btn btn--sm" data-testid="user-archive-batch" @click="batchArchiveOpen = true">
            批量建档
          </button>
          <button type="button" class="btn btn--sm btn--primary" data-testid="user-archive" @click="archiveOpen = true">
            建档
          </button>
        </span>
      </div>

      <DataTable
        test-id="users"
        :columns="columns"
        :rows="users.items"
        row-key="id"
        :loading="users.loading"
        :error="users.error"
        empty-text="未找到匹配用户，可更换手机号或状态条件"
        :selected-key="users.detail ? users.detail.id : null"
        :row-test-id="row => `user-row-${row.id}`"
        @row-click="openOrders"
        @retry="users.load()"
      >
        <template #cell-statusText="{ row }">
          <StatusPill :text="row.statusText || (row.status === 1 ? '正常' : '冻结')" :tone="statusTone(row.statusText)" />
        </template>
        <template #cell-phoneMasked="{ row }">
          <span class="row-actions">
            <button
              type="button"
              class="btn btn--sm"
              :class="row.status === 1 ? 'btn--danger' : 'btn--ghost'"
              :data-testid="`user-freeze-${row.id}`"
              @click.stop="openStatusDialog(row)"
            >
              {{ row.status === 1 ? '冻结' : '解冻' }}
            </button>
            <button type="button" class="btn btn--sm" :data-testid="`user-orders-${row.id}`" @click.stop="openOrders(row)">
              订单历史
            </button>
          </span>
        </template>
      </DataTable>

      <div class="panel__row panel__row--end pager">
        <button
          type="button"
          class="btn btn--sm"
          :disabled="users.page <= 1"
          data-testid="users-prev"
          @click="users.goToPage(users.page - 1)"
        >
          上一页
        </button>
        <span class="muted">第 {{ users.page }} / {{ users.pageCount }} 页</span>
        <button
          type="button"
          class="btn btn--sm"
          :disabled="users.page >= users.pageCount"
          data-testid="users-next"
          @click="users.goToPage(users.page + 1)"
        >
          下一页
        </button>
      </div>
    </section>

    <section v-if="users.detail || users.detailLoading" class="panel" v-reveal data-testid="user-detail">
      <div class="panel__title">
        <div>
          <h2>用户详情</h2>
          <p class="panel__hint">钱包汇总、会话数量与活动流程摘要均来自服务端脱敏视图</p>
        </div>
        <button type="button" class="btn btn--sm" data-testid="user-detail-close" @click="users.closeDetail()">关闭</button>
      </div>

      <p v-if="users.detailError" class="alert alert--error" data-testid="user-detail-error">{{ users.detailError }}</p>
      <AppSkeleton v-else-if="users.detailLoading" data-testid="user-detail-loading" variant="lines" :rows="4" />

      <ul v-if="users.detail" class="metric-list" data-testid="user-detail-metrics">
        <li>
          <span>用户</span>
          <strong>#{{ users.detail.id }} · {{ users.detail.username || '—' }}</strong>
        </li>
        <li>
          <span>手机号</span>
          <strong>{{ users.detail.phoneMasked || '—' }}</strong>
        </li>
        <li>
          <span>状态</span>
          <strong>{{ users.detail.statusText || (users.detail.status === 1 ? '正常' : '冻结') }}</strong>
        </li>
        <li>
          <span>余额 / 欠费</span>
          <strong>{{ formatAmount(users.detail.balanceCent) }} / {{ formatAmount(users.detail.debtCent) }} 元</strong>
        </li>
        <li>
          <span>有效会话</span>
          <strong>{{ formatInt(users.detail.activeSessionCount) }} 个</strong>
        </li>
        <li>
          <span>活动流程</span>
          <strong>{{ users.detail.hasActiveFlow ? '有进行中的流程' : '无' }}</strong>
        </li>
      </ul>

      <h3 class="orders-title">订单历史</h3>
      <DataTable
        test-id="user-orders"
        :columns="orderColumns"
        :rows="users.orders"
        row-key="orderNo"
        :loading="users.ordersLoading"
        :error="users.ordersError"
        empty-text="该用户暂无订单记录"
        :row-test-id="row => `user-order-${row.orderNo}`"
      />
    </section>

    <FormDialog
      v-if="archiveOpen"
      test-id="user-archive-dialog"
      title="用户建档"
      hint="账号出生即自注册形态：正常状态、零余额钱包、无密码；用户仍通过短信登录"
      :fields="archiveFields"
      submit-label="建档"
      :loading="users.saving"
      :error="archiveError || users.conflict || users.error"
      @submit="submitArchive"
      @cancel="archiveOpen = false"
    />

    <FormDialog
      v-if="batchArchiveOpen"
      test-id="user-archive-batch-dialog"
      title="批量建档"
      hint="一页最多 1000 个账号，整批同事务：任一手机号已注册则全部不创建"
      :fields="batchArchiveFields"
      submit-label="批量建档"
      :loading="users.saving"
      :error="rosterError || users.conflict || users.error"
      @submit="submitBatchArchive"
      @cancel="batchArchiveOpen = false"
    />

    <ConfirmDialog
      v-if="statusTarget"
      :title="statusTarget.status === 1 ? '冻结用户' : '解冻用户'"
      :hint="statusHint"
      :danger="statusTarget.status === 1"
      :confirm-label="statusTarget.status === 1 ? '确认冻结' : '确认解冻'"
      :loading="users.saving"
      :error="users.error"
      @confirm="submitStatus"
      @cancel="statusTarget = null"
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

.orders-title {
  margin: var(--ncs-s-4) 0 var(--ncs-s-2);
  font-size: var(--ncs-fs-sm);
  color: var(--ncs-muted);
  font-weight: 600;
}
</style>
