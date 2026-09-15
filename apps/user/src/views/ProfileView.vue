<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import * as chargingApi from '@/api/charging'
import { randomId } from '@/api/http'
import { formatYuan } from '@/services/coordinate'
import { fetchAvatarObjectUrl } from '@/api/auth'
import { useAuthStore } from '@/stores/auth'

/** 我的：验证码登录 / 退出、资料修改、钱包余额与充值。 */
const auth = useAuthStore()

const phone = ref('')
const smsCode = ref('')
const nickname = ref('')
const avatarFile = ref(null)
const avatarObjectUrl = ref('')
const rechargeAmountYuan = ref('50')
const notice = ref('')

let pendingRechargeKey = null
let pendingAvatarKey = null

const canSubmitLogin = computed(() => /^1\d{10}$/.test(phone.value.trim()) && /^\d{6}$/.test(smsCode.value.trim()))
const rechargeAmountCent = computed(() => Math.round(Number(rechargeAmountYuan.value) * 100))

onMounted(async () => {
  if (!auth.isLoggedIn) return
  await auth.refreshProfile()
  await auth.refreshWallet()
  nickname.value = auth.user?.nickname || ''
  await loadAvatar()
})

onBeforeUnmount(() => {
  if (avatarObjectUrl.value) URL.revokeObjectURL(avatarObjectUrl.value)
})

/** 头像接口需要 Bearer 令牌，因此取回内容后使用 blob URL，避免 <img> 请求未授权。 */
async function loadAvatar() {
  if (!auth.avatarUrl) return
  const url = await fetchAvatarObjectUrl()
  if (!url) return
  if (avatarObjectUrl.value) URL.revokeObjectURL(avatarObjectUrl.value)
  avatarObjectUrl.value = url
}

async function requestCode() {
  notice.value = ''
  if (!/^1\d{10}$/.test(phone.value.trim())) {
    auth.error = '请输入 11 位手机号'
    return
  }
  const data = await auth.requestCode(phone.value.trim(), 'LOGIN')
  if (data?.developmentCode) notice.value = `开发环境模拟验证码：${data.developmentCode}`
}

async function loginOrRegister() {
  notice.value = ''
  if (!canSubmitLogin.value) {
    auth.error = '请输入手机号与 6 位验证码'
    return
  }
  // 手机号不存在时服务端自动注册，因此登录与注册共用同一入口。
  await auth.loginWithSms({ phone: phone.value.trim(), smsCode: smsCode.value.trim() })
}

async function saveNickname() {
  const value = nickname.value.trim()
  if (!value) {
    auth.error = '昵称不能为空'
    return
  }
  await auth.updateNickname(value)
}

async function uploadAvatar() {
  const file = avatarFile.value
  if (!file) return
  pendingAvatarKey = pendingAvatarKey || randomId()
  const done = await auth.uploadAvatar(file)
  if (done) {
    pendingAvatarKey = null
    await loadAvatar()
  }
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

/** 未设置头像（404 或未登录）时使用仓库自带的场景图作为占位。 */
const avatarSrc = computed(() => avatarObjectUrl.value || '/charging-scene.png')
</script>

<template>
  <section class="view profile-view" data-testid="profile-view">
    <div v-if="!auth.isLoggedIn" class="panel" data-testid="login-panel">
      <h2>登录 / 注册</h2>
      <p class="muted">使用手机号验证码登录；未注册的手机号会自动创建账号。</p>

      <form class="stack-form" @submit.prevent="loginOrRegister">
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

        <div class="panel__row">
          <input data-testid="profile-avatar-file" type="file" accept="image/png,image/jpeg,image/bmp" @change="avatarFile = $event.target.files?.[0] || null" />
          <button type="button" class="btn" data-testid="profile-avatar-upload" :disabled="!avatarFile || auth.loading" @click="uploadAvatar">上传头像</button>
        </div>
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
