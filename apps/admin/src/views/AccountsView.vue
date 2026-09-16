<script setup>
/**
 * 管理员账号（接口文档 §6.8–§6.11）。
 *
 * 账号管理只向超级管理员开放；创建账号与停启用属于敏感操作（幂等键 + 版本校验），
 * 创建出来的账号角色固定为 OPERATOR
 * 且 mustChangePassword=true。
 * 页面同时提供“修改本人密码”，用于完成首次登录改密与定期换密。
 */
import { computed, onMounted, ref } from 'vue'
import ConfirmDialog from '@/components/ConfirmDialog.vue'
import DataTable from '@/components/DataTable.vue'
import FormDialog from '@/components/FormDialog.vue'
import StatusPill from '@/components/StatusPill.vue'
import { ADMIN_STATUS } from '@/utils/domain'
import { useAccountsStore } from '@/stores/accounts'
import { useAuthStore } from '@/stores/auth'

const accounts = useAccountsStore()
const auth = useAuthStore()

const createOpen = ref(false)
const statusTarget = ref(null)

const currentPassword = ref('')
const newPassword = ref('')
const confirmPassword = ref('')
const passwordError = ref('')
const passwordLoading = ref(false)
const passwordNotice = ref('')

onMounted(() => {
  if (auth.isSuperAdmin) accounts.load()
})

const columns = [
  { key: 'id', label: 'ID', align: 'right' },
  { key: 'username', label: '账号' },
  { key: 'roles', label: '角色', format: value => (Array.isArray(value) ? value.join(' / ') : '—') },
  { key: 'status', label: '状态', format: value => (value === 1 ? '启用' : '停用') },
  { key: 'mustChangePassword', label: '首次改密', format: value => (value === true ? '待修改' : '已完成') },
  { key: 'version', label: '版本', align: 'right' }
]

const createFields = computed(() => [
  { key: 'username', label: '账号', required: true, minLength: 3, maxLength: 32, help: '3~32 位字母、数字或下划线，全局唯一' },
  { key: 'password', label: '初始密码', type: 'password', required: true, minLength: 10, maxLength: 128, help: '10~128 位，仅用于首次登录' },
  { key: 'reason', label: '创建原因', required: true, minLength: 2, maxLength: 200, help: '写入审计日志' }
])

const statusHint = computed(() => {
  if (!statusTarget.value) return ''
  const action = statusTarget.value.status === 1 ? '停用' : '启用'
  return `确认${action}管理员 ${statusTarget.value.username}？停用会立即撤销该账号全部会话并阻止登录；管理员不得停用本人账号。`
})

function statusTone(status) {
  const found = ADMIN_STATUS.find(item => item.value === status)
  return found ? found.tone : 'muted'
}

async function submitCreate(values) {
  const ok = await accounts.create(values)
  if (ok) createOpen.value = false
}

async function submitStatus({ reason }) {
  if (!statusTarget.value) return
  const target = statusTarget.value
  const nextStatus = target.status === 1 ? 0 : 1
  const ok = await accounts.setStatus(target, nextStatus, reason)
  if (ok) statusTarget.value = null
}

async function submitPassword() {
  passwordError.value = ''
  passwordNotice.value = ''
  if (newPassword.value.length < 10 || newPassword.value.length > 128) {
    passwordError.value = '新密码长度必须为 10~128 位'
    return
  }
  if (newPassword.value !== confirmPassword.value) {
    passwordError.value = '两次输入的新密码不一致'
    return
  }
  passwordLoading.value = true
  try {
    await auth.changeOwnPassword({
      currentPassword: currentPassword.value,
      newPassword: newPassword.value
    })
    passwordNotice.value = '密码已更新，其他终端会话已失效'
    currentPassword.value = ''
    newPassword.value = ''
    confirmPassword.value = ''
  } catch (error) {
    passwordError.value = error?.userMessage || '修改密码失败，请检查当前密码'
  } finally {
    passwordLoading.value = false
  }
}
</script>

<template>
  <div class="view">
    <p v-if="auth.mustChangePassword" class="alert" data-testid="accounts-must-change-hint">
      当前账号处于首次登录状态（mustChangePassword=true），请先在下方修改密码。
    </p>
    <p v-if="!auth.isSuperAdmin" class="alert" data-testid="accounts-permission-hint">
      管理员账号管理仅向超级管理员开放。
    </p>
    <p v-if="accounts.notice" class="alert alert--ok" data-testid="accounts-notice">{{ accounts.notice }}</p>
    <p v-if="accounts.error" class="alert alert--error" data-testid="accounts-error-banner">{{ accounts.error }}</p>
    <p v-if="accounts.conflict" class="alert" data-testid="accounts-conflict">{{ accounts.conflict }}</p>

    <section v-if="auth.isSuperAdmin" class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>管理员账号</h2>
          <p class="panel__hint">
            共 {{ accounts.total }} 个账号 · 启用 {{ accounts.enabledCount }} 个 · 待首次改密
            {{ accounts.pendingPasswordCount }} 个
          </p>
        </div>
        <button type="button" class="btn btn--sm btn--primary" data-testid="account-create" @click="createOpen = true">
          新增管理员
        </button>
      </div>

      <DataTable
        test-id="accounts"
        :columns="columns"
        :rows="accounts.items"
        row-key="id"
        :loading="accounts.loading"
        :error="accounts.error"
        empty-text="暂无管理员账号"
        :row-test-id="row => `account-row-${row.id}`"
        @retry="accounts.load()"
      >
        <template #cell-status="{ row }">
          <StatusPill :text="row.status === 1 ? '启用' : '停用'" :tone="statusTone(row.status)" :test-id="`account-state-${row.id}`" />
        </template>
        <template #cell-version="{ row }">
          <button
            type="button"
            class="btn btn--sm"
            :class="row.status === 1 ? 'btn--danger' : 'btn--ghost'"
            :data-testid="`account-toggle-${row.id}`"
            @click.stop="statusTarget = row"
          >
            {{ row.status === 1 ? '停用' : '启用' }}
          </button>
        </template>
      </DataTable>

      <div class="panel__row panel__row--end pager">
        <button
          type="button"
          class="btn btn--sm"
          :disabled="accounts.page <= 1"
          data-testid="accounts-prev"
          @click="accounts.goToPage(accounts.page - 1)"
        >
          上一页
        </button>
        <span class="muted">第 {{ accounts.page }} / {{ accounts.pageCount }} 页</span>
        <button
          type="button"
          class="btn btn--sm"
          :disabled="accounts.page >= accounts.pageCount"
          data-testid="accounts-next"
          @click="accounts.goToPage(accounts.page + 1)"
        >
          下一页
        </button>
      </div>
    </section>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>修改本人密码</h2>
          <p class="panel__hint">
            当前账号 {{ auth.displayName }} · 需要当前密码；成功后清除首次改密标记并使其他终端会话失效
          </p>
        </div>
      </div>

      <form class="password-form" data-testid="me-password-form" @submit.prevent="submitPassword" @keydown.enter.prevent="submitPassword">
        <label class="field">
          <span>当前密码</span>
          <input v-model="currentPassword" type="password" autocomplete="current-password" data-testid="me-password-current" />
        </label>
        <label class="field">
          <span>新密码</span>
          <input v-model="newPassword" type="password" autocomplete="new-password" placeholder="10~128 位" data-testid="me-password-new" />
        </label>
        <label class="field">
          <span>确认新密码</span>
          <input v-model="confirmPassword" type="password" autocomplete="new-password" data-testid="me-password-confirm" />
        </label>
        <button
          type="button"
          class="btn btn--primary"
          :disabled="passwordLoading"
          data-testid="me-password-submit"
          @click="submitPassword"
        >
          {{ passwordLoading ? '提交中…' : '修改密码' }}
        </button>
      </form>

      <p v-if="passwordError" class="alert alert--error" data-testid="me-password-error">{{ passwordError }}</p>
      <p v-else-if="passwordNotice" class="alert alert--ok" data-testid="me-password-notice">{{ passwordNotice }}</p>
    </section>

    <FormDialog
      v-if="createOpen"
      test-id="account-create-dialog"
      title="新增管理员账号"
      hint="新建账号角色固定为运营管理员 OPERATOR，状态正常且必须首次登录修改密码"
      :fields="createFields"
      submit-label="创建账号"
      :loading="accounts.saving"
      :error="accounts.error"
      @submit="submitCreate"
      @cancel="createOpen = false"
    />

    <ConfirmDialog
      v-if="statusTarget"
      :title="statusTarget.status === 1 ? '停用管理员' : '启用管理员'"
      :hint="statusHint"
      :danger="statusTarget.status === 1"
      :confirm-label="statusTarget.status === 1 ? '确认停用' : '确认启用'"
      :loading="accounts.saving"
      :error="accounts.error"
      @confirm="submitStatus"
      @cancel="statusTarget = null"
    />
  </div>
</template>

<style scoped>
.password-form {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
  gap: var(--ncs-s-3);
  align-items: end;
}

.password-form button {
  height: 38px;
}

.pager {
  justify-content: flex-end;
  margin-top: var(--ncs-s-3);
}
</style>
