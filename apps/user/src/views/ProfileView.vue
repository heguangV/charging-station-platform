<script setup>
import { computed, onMounted, ref } from 'vue'
import * as chargingApi from '@/api/charging'
import { randomId } from '@/api/http'
import { formatYuan } from '@/services/coordinate'
import { useAuthStore } from '@/stores/auth'

/**
 * 我的：验证码登录 / 退出、资料修改、钱包余额与充值。
 * 头像上传在 Go 契约中暂缺（B-01 待决策），入口显式提示未开放。
 */
const auth = useAuthStore()

const phone = ref('')
const smsCode = ref('')
const nickname = ref('')
const rechargeAmountYuan = ref('50')
const notice = ref('')
/** 登录面板模式：code = 验证码登录，register = 用户名密码注册（Go 契约已提供）。 */
const loginMode = ref('code')
const regUsername = ref('')
const regPhone = ref('')
const regPassword = ref('')
const regSmsCode = ref('')
const avatarUrlInput = ref('')

let pendingRechargeKey = null

const canSubmitLogin = computed(() => /^1\d{10}$/.test(phone.value.trim()) && /^\d{6}$/.test(smsCode.value.trim()))
const canSubmitRegister = computed(
  () => /^1\d{10}$/.test(regPhone.value.trim()) && regPassword.value.length >= 8 && /^\d{6}$/.test(regSmsCode.value.trim())
)
const rechargeAmountCent = computed(() => Math.round(Number(rechargeAmountYuan.value) * 100))

onMounted(async () => {
  if (!auth.isLoggedIn) return
  await auth.refreshProfile()
  await auth.refreshWallet()
  nickname.value = auth.displayName === '未登录' ? '' : auth.displayName
  avatarUrlInput.value = auth.avatarUrl || ''
})

async function requestCode() {
  notice.value = ''
  if (!/^1\d{10}$/.test(phone.value.trim())) {
    auth.error = '请输入 11 位手机号'
    return
  }
  const data = await auth.requestCode(phone.value.trim())
  if (data?.developmentCode) notice.value = `开发环境模拟验证码：${data.developmentCode}`
}

async function loginOrRegister() {
  notice.value = ''
  if (!canSubmitLogin.value) {
    auth.error = '请输入手机号与 6 位验证码'
    return
  }
  // 手机号不存在时服务端自动注册，因此验证码登录与注册共用同一入口。
  await auth.loginWithSms({ phone: phone.value.trim(), smsCode: smsCode.value.trim() })
}

/** 用户名密码注册（POST /auth/user/register，成功即建立会话）。 */
async function submitRegister() {
  notice.value = ''
  if (!canSubmitRegister.value) {
    auth.error = '注册需要 11 位手机号、8 位以上密码与 6 位验证码'
    return
  }
  await auth.register({
    username: regUsername.value.trim() || undefined,
    phone: regPhone.value.trim(),
    password: regPassword.value,
    smsCode: regSmsCode.value.trim()
  })
}

/** 获取注册用验证码（与登录共用同一短信端点）。 */
async function requestRegisterCode() {
  notice.value = ''
  if (!/^1\d{10}$/.test(regPhone.value.trim())) {
    auth.error = '请输入 11 位手机号'
    return
  }
  const data = await auth.requestCode(regPhone.value.trim())
  if (data?.developmentCode) notice.value = `开发环境模拟验证码：${data.developmentCode}`
}

/** 头像地址（Go 契约 avatarUrl 字符串，≤512；空串表示无头像）。 */
async function saveAvatarUrl() {
  const value = avatarUrlInput.value.trim()
  if (value.length > 512) {
    auth.error = '头像地址不能超过 512 个字符'
    return
  }
  await auth.updateAvatarUrl(value)
}

async function saveNickname() {
  const value = nickname.value.trim()
  if (!value) {
    auth.error = '昵称不能为空'
    return
  }
  await auth.updateNickname(value)
}

async function recharge() {
  const amountCent = rechargeAmountCent.value
  if (!Number.isFinite(amountCent) || amountCent < 1 || amountCent > 1000000) {
    auth.error = '充值金额需在 0.01 元到 10000 元之间'
    return
  }
  // 失败重试复用同一幂等键，避免重复入账。
  pendingRechargeKey = pendingRechargeKey || randomId()
  try {
    await chargingApi.rechargeWallet(amountCent, pendingRechargeKey)
    pendingRechargeKey = null
    notice.value = '充值成功'
    await auth.refreshWallet()
    await auth.refreshProfile()
  } catch (error) {
    auth.error = error?.userMessage || '充值失败，请稍后重试'
  }
}

/** 头像：设置了 avatarUrl 用之，否则回落仓库自带的场景图。 */
const avatarSrc = computed(() => auth.avatarUrl || '/charging-scene.png')
</script>

<template>
  <section class="view profile-view" data-testid="profile-view">
    <div v-if="!auth.isLoggedIn" class="panel" data-testid="login-panel">
      <h2>登录 / 注册</h2>
      <p class="muted">使用手机号验证码登录；未注册的手机号会自动创建账号。</p>

      <div class="panel__row">
        <button type="button" class="chip" :class="{ 'is-active': loginMode === 'code' }" data-testid="login-mode-code" @click="loginMode = 'code'">验证码登录</button>
        <button type="button" class="chip" :class="{ 'is-active': loginMode === 'register' }" data-testid="login-mode-register" @click="loginMode = 'register'">账号注册</button>
      </div>

      <form v-if="loginMode === 'code'" class="stack-form" @submit.prevent="loginOrRegister">
        <label class="field">
          <span>手机号</span>
          <input v-model="phone" data-testid="login-phone" type="tel" maxlength="11" placeholder="13800138000" />
        </label>
        <label class="field">
          <span>验证码</span>
          <input v-model="smsCode" data-testid="login-code" type="text" maxlength="6" placeholder="6 位数字" />
        </label>
        <div class="panel__row">
          <button type="button" class="btn" data-testid="login-request-code" :disabled="auth.codeLoading" @click="requestCode">
            {{ auth.codeLoading ? '发送中…' : '获取验证码' }}
          </button>
          <button type="submit" class="btn btn--primary" data-testid="login-submit" :disabled="auth.loading">
            {{ auth.loading ? '登录中…' : '登录' }}
          </button>
        </div>
      </form>

      <form v-else class="stack-form" @submit.prevent="submitRegister">
        <label class="field">
          <span>昵称（可选）</span>
          <input v-model="regUsername" data-testid="register-username" type="text" maxlength="20" placeholder="不填则使用手机号命名" />
        </label>
        <label class="field">
          <span>手机号</span>
          <input v-model="regPhone" data-testid="register-phone" type="tel" maxlength="11" placeholder="13800138000" />
        </label>
        <label class="field">
          <span>密码（至少 8 位）</span>
          <input v-model="regPassword" data-testid="register-password" type="password" minlength="8" maxlength="128" placeholder="至少 8 位" />
        </label>
        <label class="field">
          <span>验证码</span>
          <input v-model="regSmsCode" data-testid="register-code" type="text" maxlength="6" placeholder="6 位数字" />
        </label>
        <div class="panel__row">
          <button type="button" class="btn" data-testid="register-request-code" :disabled="auth.codeLoading" @click="requestRegisterCode">
            {{ auth.codeLoading ? '发送中…' : '获取验证码' }}
          </button>
          <button type="submit" class="btn btn--primary" data-testid="register-submit" :disabled="auth.loading">
            {{ auth.loading ? '注册中…' : '注册并登录' }}
          </button>
        </div>
      </form>

      <p v-if="auth.developmentCode" class="alert" data-testid="login-dev-code">开发环境模拟验证码：{{ auth.developmentCode }}</p>
      <p v-if="auth.error" class="alert alert--error" data-testid="login-error">{{ auth.error }}</p>
      <p v-if="notice" class="alert" data-testid="login-notice">{{ notice }}</p>
    </div>

    <template v-else>
      <div class="panel profile-card" data-testid="profile-card">
        <img class="profile-card__avatar" :src="avatarSrc" alt="用户头像" data-testid="profile-avatar" />
        <div>
          <h2 data-testid="profile-name">{{ auth.displayName }}</h2>
          <p class="muted" data-testid="profile-phone">{{ auth.phoneMasked || '手机号已脱敏' }}</p>
          <p class="muted">注册时间：{{ auth.user?.registeredAt ? new Date(auth.user.registeredAt * 1000).toLocaleString('zh-CN', { hour12: false }) : '—' }}</p>
        </div>
      </div>

      <div class="panel" data-testid="profile-wallet">
        <h3>钱包</h3>
        <ul class="metric-list">
          <li><span>余额</span><strong data-testid="profile-balance">{{ formatYuan(auth.balanceCent) }} 元</strong></li>
          <li><span>欠费</span><strong data-testid="profile-debt">{{ formatYuan(auth.debtCent) }} 元</strong></li>
          <li><span>可用</span><strong>{{ formatYuan(auth.availableCent) }} 元</strong></li>
        </ul>
        <div class="panel__row">
          <input v-model="rechargeAmountYuan" data-testid="profile-recharge-amount" type="number" min="0.01" step="0.01" />
          <button type="button" class="btn btn--primary" data-testid="profile-recharge" :disabled="auth.loading" @click="recharge">充值</button>
          <button type="button" class="btn" data-testid="profile-refresh" @click="auth.refreshWallet()">刷新钱包</button>
        </div>
      </div>

      <div class="panel" data-testid="profile-editor">
        <h3>资料</h3>
        <form class="stack-form" @submit.prevent="saveNickname">
          <label class="field">
            <span>昵称</span>
            <input v-model="nickname" data-testid="profile-nickname" type="text" maxlength="20" />
          </label>
          <button type="submit" class="btn btn--primary" data-testid="profile-save-nickname" :disabled="auth.loading">保存昵称</button>
        </form>

        <form class="stack-form" @submit.prevent="saveAvatarUrl">
          <label class="field">
            <span>头像地址（Go 契约以 URL 字段存储，最长 512；清空保存表示无头像）</span>
            <input v-model="avatarUrlInput" data-testid="profile-avatar-url" type="url" maxlength="512" placeholder="https://example.com/avatar.png" />
          </label>
          <button type="submit" class="btn" data-testid="profile-avatar-save" :disabled="auth.loading">保存头像地址</button>
        </form>
      </div>

      <p v-if="auth.error" class="alert alert--error" data-testid="profile-error">{{ auth.error }}</p>
      <p v-if="notice || auth.notice" class="alert" data-testid="profile-notice">{{ notice || auth.notice }}</p>

      <div class="panel panel__row">
        <RouterLink class="btn" to="/orders" data-testid="profile-orders-link">我的订单</RouterLink>
        <RouterLink class="btn" to="/charging" data-testid="profile-charging-link">充电流程</RouterLink>
        <button type="button" class="btn btn--danger" data-testid="profile-logout" @click="auth.logout()">退出登录</button>
      </div>
    </template>
  </section>
</template>

<style scoped>
/* 资料卡：头像加一圈品牌描边，让页面第一眼有“账户”视觉重心 */
.profile-card {
  display: flex;
  gap: var(--ncs-s-4);
  align-items: center;
  background:
    linear-gradient(140deg, rgba(13, 122, 111, 0.06), transparent 55%),
    var(--ncs-surface);
}

.profile-card__avatar {
  width: 68px;
  height: 68px;
  border-radius: 50%;
  object-fit: cover;
  background: var(--ncs-surface-3);
  border: 2px solid var(--ncs-surface);
  box-shadow:
    0 0 0 2px var(--ncs-brand-soft),
    var(--ncs-shadow-2);
  transition: transform var(--ncs-dur-2) var(--ncs-ease-spring);
}

.profile-card:hover .profile-card__avatar {
  transform: scale(1.03);
}

.profile-card h2 {
  font-size: var(--ncs-fs-lg);
  letter-spacing: -0.02em;
}

/* 余额/欠费两行用大号等宽数字，刷新时数值不抖动 */
[data-testid='profile-wallet'] strong {
  font-variant-numeric: tabular-nums;
  font-size: var(--ncs-fs-md);
}
</style>
