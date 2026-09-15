<script setup>
/**
 * 管理员登录页（接口文档 §6.1）。
 *
 * 覆盖两个真实流程：
 * 1. 登录锁定：连续失败 5 次后服务端返回 RATE_LIMITED，页面进入 30 秒倒计时并禁用提交；
 * 2. 首次登录改密：响应里 mustChangePassword=true 时，先完成 §6.11 修改密码再进入控制台。
 */
import { computed, onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import NavIcon from '@/components/NavIcon.vue'
import { useAuthStore, LOGIN_LOCK_SECONDS } from '@/stores/auth'

const route = useRoute()
const router = useRouter()
const auth = useAuthStore()

const username = ref('')
const password = ref('')
const deviceId = ref(auth.deviceId)
const currentPassword = ref('')
const newPassword = ref('')
const confirmPassword = ref('')
const changeError = ref('')
const changeLoading = ref(false)

/** 已经登录但必须先改密时，直接进入改密步骤。 */
const requireChange = computed(() => auth.isLoggedIn && auth.mustChangePassword)

const submitLabel = computed(() => {
  if (auth.isLocked) return `已锁定 ${auth.lockedSeconds} 秒`
  return auth.loading ? '登录中…' : '登录'
})

onMounted(() => {
  if (auth.sessionExpired) auth.clearSession('')
})

function redirectAfterLogin() {
  const target = typeof route.query.redirect === 'string' && route.query.redirect !== '' ? route.query.redirect : '/'
  router.replace(target)
}

async function submitLogin() {
  if (auth.isLocked) return
  auth.deviceId = deviceId.value.trim() || 'ncs-admin-web'
  const ok = await auth.login({ username: username.value.trim(), password: password.value })
  if (!ok) return
  if (auth.mustChangePassword) {
    // 首次登录：用刚输入的密码预填当前密码，减少一次输入。
    currentPassword.value = password.value
    password.value = ''
    return
  }
  redirectAfterLogin()
}

async function submitChange() {
  changeError.value = ''
  if (newPassword.value.length < 10 || newPassword.value.length > 128) {
    changeError.value = '新密码长度必须为 10~128 位'
    return
  }
  if (newPassword.value !== confirmPassword.value) {
    changeError.value = '两次输入的新密码不一致'
    return
  }
  changeLoading.value = true
  try {
    await auth.changeOwnPassword({
      currentPassword: currentPassword.value,
      newPassword: newPassword.value
    })
    redirectAfterLogin()
  } catch (error) {
    changeError.value = error?.userMessage || '修改密码失败，请检查当前密码'
  } finally {
    changeLoading.value = false
  }
}
</script>

<template>
  <div class="login-view" data-testid="login-view">
    <div class="login-view__card panel overlay-rise">
      <div class="login-view__brand">
        <span class="login-view__logo" aria-hidden="true"><NavIcon name="charger" :size="22" /></span>
        <div>
          <strong>NCS 充电运营中心</strong>
          <span class="login-view__subtitle">管理端 · 运营 / 运维</span>
        </div>
      </div>

      <template v-if="!requireChange">
        <p class="login-view__hint">使用管理员账号登录。连续 5 次密码错误将锁定 {{ LOGIN_LOCK_SECONDS }} 秒。</p>

        <form class="stack-form" @submit.prevent="submitLogin" @keydown.enter.prevent="submitLogin">
          <label class="field">
            <span>账号</span>
            <input
              v-model="username"
              type="text"
              autocomplete="username"
              placeholder="请输入管理员账号"
              data-testid="login-username"
            />
          </label>
          <label class="field">
            <span>密码</span>
            <input
              v-model="password"
              type="password"
              autocomplete="current-password"
              placeholder="请输入登录密码"
              data-testid="login-password"
            />
          </label>
          <label class="field">
            <span>设备标识</span>
            <input v-model="deviceId" type="text" placeholder="用于会话审计，可留默认值" data-testid="login-device" />
          </label>

          <button
            type="button"
            class="btn btn--primary"
            :disabled="auth.loading || auth.isLocked"
            data-testid="login-submit"
            @click="submitLogin"
          >
            {{ submitLabel }}
          </button>
        </form>

        <p v-if="auth.error" class="alert alert--error" data-testid="login-error">
          {{ auth.error }}
          <span v-if="auth.isLocked" class="login-view__countdown" data-testid="login-lock-countdown">
            剩余 {{ auth.lockedSeconds }} 秒
          </span>
        </p>
        <p v-else-if="auth.notice" class="alert alert--ok" data-testid="login-notice">{{ auth.notice }}</p>
      </template>

      <template v-else>
        <p class="login-view__hint">
          首次登录必须修改初始密码（{{ auth.displayName }}）。新密码长度 10~128 位，修改后其他终端会话立即失效。
        </p>

        <form class="stack-form" @submit.prevent="submitChange" @keydown.enter.prevent="submitChange">
          <label class="field">
            <span>当前密码</span>
            <input v-model="currentPassword" type="password" data-testid="login-current-password" />
          </label>
          <label class="field">
            <span>新密码</span>
            <input v-model="newPassword" type="password" placeholder="至少 10 位" data-testid="login-new-password" />
          </label>
          <label class="field">
            <span>确认新密码</span>
            <input v-model="confirmPassword" type="password" data-testid="login-confirm-password" />
          </label>

          <button
            type="button"
            class="btn btn--primary"
            :disabled="changeLoading"
            data-testid="login-change-submit"
            @click="submitChange"
          >
            {{ changeLoading ? '提交中…' : '修改密码并进入控制台' }}
          </button>
        </form>

        <p v-if="changeError" class="alert alert--error" data-testid="login-change-error">{{ changeError }}</p>
      </template>
    </div>
  </div>
</template>

<style scoped>
.login-view {
  display: grid;
  place-items: center;
  min-height: 100vh;
  min-height: 100dvh;
  padding: var(--ncs-s-4);
}

.login-view__card {
  width: min(430px, 100%);
  padding: var(--ncs-s-6);
  display: flex;
  flex-direction: column;
  gap: var(--ncs-s-4);
}

.login-view__brand {
  display: flex;
  align-items: center;
  gap: 12px;
}

.login-view__logo {
  display: grid;
  place-items: center;
  width: 44px;
  height: 44px;
  border-radius: var(--ncs-r-sm);
  color: #fff;
  background: linear-gradient(140deg, var(--ncs-brand-bright), var(--ncs-brand-strong));
  box-shadow: 0 8px 18px rgba(13, 122, 111, 0.28);
}

.login-view__brand strong {
  display: block;
  font-size: var(--ncs-fs-md);
  letter-spacing: -0.01em;
}

.login-view__subtitle {
  display: block;
  font-size: var(--ncs-fs-xs);
  color: var(--ncs-muted);
  letter-spacing: 0.12em;
}

.login-view__hint {
  margin: 0;
  font-size: var(--ncs-fs-sm);
  color: var(--ncs-muted);
}

.login-view__countdown {
  margin-left: var(--ncs-s-2);
  font-variant-numeric: tabular-nums;
  font-weight: 650;
}
</style>
