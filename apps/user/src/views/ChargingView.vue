<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import * as chargingApi from '@/api/charging'
import * as orderApi from '@/api/order'
import { randomId } from '@/api/http'
import { formatDuration, formatEnergy, formatYuan } from '@/services/coordinate'
import AppSkeleton from '@/components/AppSkeleton.vue'
import { useValueFlash } from '@/composables/useValueFlash'
import { useAuthStore } from '@/stores/auth'

/**
 * 充电页（Go 订单模型）：创建订单即绑定设备并锁定价格快照，START/STOP 是 202
 * 异步设备命令，结算由设备回执驱动。前端只轮询订单详情展示状态与累计量，
 * 结束充电 = POST stop 后轮询至 COMPLETED 再展示小票。
 */
const router = useRouter()
const auth = useAuthStore()

const order = ref(null)
const receipt = ref(null)
const loading = ref(false)
const busy = ref(false)
const error = ref('')
const actionError = ref('')
const rechargeAmountYuan = ref('50')

const POLL_INTERVAL_MS = 3000
let pollTimer = null
let pendingRechargeKey = null

const status = computed(() => order.value?.status ?? '')
const isActive = computed(() => order.value !== null && order.value.active === true)
const energyMwh = computed(() => (Number.isFinite(order.value?.energyMwh) ? order.value.energyMwh : 0))
const amountCent = computed(() => (Number.isFinite(order.value?.amountCent) ? order.value.amountCent : 0))
const durationSec = computed(() => (Number.isFinite(order.value?.durationSec) ? order.value.durationSec : 0))

/**
 * 充电量/金额变化时用一次短暂高亮提示“数据刚更新”。
 * 只切换 class，不参与任何格式化，因此断言到的文本始终是最终值。
 */
const { flashing: energyFlashing } = useValueFlash(energyMwh)
const { flashing: amountFlashing } = useValueFlash(amountCent)
const { flashing: balanceFlashing } = useValueFlash(() => auth.balanceCent)

onMounted(async () => {
  if (!auth.isLoggedIn) return
  await auth.refreshWallet()
  await refreshOrder()
})

onBeforeUnmount(stopPolling)

function stopPolling() {
  if (pollTimer) {
    clearInterval(pollTimer)
    pollTimer = null
  }
}

function startPolling() {
  if (pollTimer) return
  pollTimer = setInterval(() => {
    if (order.value?.orderNo && order.value.active) void refreshOrder()
  }, POLL_INTERVAL_MS)
}

async function refreshOrder() {
  if (!auth.isLoggedIn) return
  loading.value = true
  error.value = ''
  try {
    const previous = order.value
    const active = await chargingApi.fetchActiveOrder()
    order.value = active
    if (active) {
      // STARTING/STOPPING 阶段等待设备回执，CHARGING 阶段跟踪累计电量与金额。
      startPolling()
    } else {
      stopPolling()
      // 轮询期间订单离开活动集合：此前进行中的订单已到终态，拉取小票。
      if (previous?.orderNo && previous.active) await loadReceipt(previous.orderNo)
    }
  } catch (caught) {
    error.value = caught?.userMessage || '充电状态加载失败，请稍后重试'
  } finally {
    loading.value = false
  }
}

async function runAction(action) {
  if (!order.value) return
  busy.value = true
  actionError.value = ''
  const orderNo = order.value.orderNo
  try {
    if (action === 'cancel') {
      await chargingApi.cancelOrder(orderNo, randomId())
    } else if (action === 'start') {
      await chargingApi.startOrder(orderNo, randomId())
    } else if (action === 'stop') {
      await chargingApi.stopOrder(orderNo, randomId())
    }
    await refreshOrder()
  } catch (caught) {
    actionError.value = caught?.userMessage || '操作失败，请稍后重试'
  } finally {
    busy.value = false
  }
}

/** 小票：订单离开活动集合且终态为 COMPLETED 时展示；其余终态直接回空闲页。 */
async function loadReceipt(orderNo) {
  // 小票属于订单接口组（@/api/order，与"我的订单"页同一入口），不在 @/api/charging 中。
  const detail = await orderApi.fetchOrderReceipt(orderNo)
  stopPolling()
  order.value = null
  if (detail?.status === 'COMPLETED') {
    receipt.value = detail
    await auth.refreshWallet()
    await auth.refreshProfile()
  }
}

async function recharge() {
  const yuan = Number(rechargeAmountYuan.value)
  const amountCent = Math.round(yuan * 100)
  if (!Number.isFinite(amountCent) || amountCent < 1 || amountCent > 1000000) {
    actionError.value = '充值金额需在 0.01 元到 10000 元之间'
    return
  }
  busy.value = true
  actionError.value = ''
  // 超时重试必须复用同一幂等键，避免重复充值。
  pendingRechargeKey = pendingRechargeKey || randomId()
  try {
    await chargingApi.rechargeWallet(amountCent, pendingRechargeKey)
    pendingRechargeKey = null
    await auth.refreshWallet()
    await auth.refreshProfile()
  } catch (caught) {
    actionError.value = caught?.userMessage || '充值失败，请稍后重试'
  } finally {
    busy.value = false
  }
}

function goHome() {
  void router.push('/')
}
</script>

<template>
  <section class="view charging-view" data-testid="charging-view">
    <div v-if="!auth.isLoggedIn" class="panel" data-testid="charging-login-required">
      <p>充电流程需要登录后使用。</p>
      <RouterLink class="btn btn--primary" to="/profile" data-testid="charging-login-link">去登录</RouterLink>
    </div>

    <template v-else>
      <div v-reveal class="panel wallet-bar" data-testid="charging-wallet">
        <p>
          余额
          <strong class="value-target" :class="{ 'value-flash': balanceFlashing }" data-testid="charging-balance">{{ formatYuan(auth.balanceCent) }}</strong>
          元
        </p>
        <div class="inline-form">
          <input v-model="rechargeAmountYuan" data-testid="charging-recharge-amount" type="number" min="0.01" step="0.01" />
          <button type="button" class="btn" data-testid="charging-recharge" :disabled="busy" @click="recharge">充值</button>
        </div>
      </div>

      <p v-if="error" class="alert alert--error" data-testid="charging-error" role="alert">
        {{ error }}
        <button type="button" class="btn" data-testid="charging-retry" @click="refreshOrder">重试</button>
      </p>
      <div v-if="loading" data-testid="charging-loading" role="status">
        <span class="sr-only">正在加载充电状态…</span>
        <AppSkeleton variant="cards" :rows="2" />
      </div>
      <p v-if="actionError" class="alert alert--error" data-testid="charging-action-error" role="alert">{{ actionError }}</p>

      <div v-if="receipt" v-reveal class="panel" data-testid="charging-receipt">
        <h2>结算小票</h2>
        <ul class="metric-list">
          <li><span>订单号</span><strong data-testid="receipt-order-no">{{ receipt.orderNo }}</strong></li>
          <li><span>站点</span><strong>{{ receipt.stationName }}</strong></li>
          <li><span>设备</span><strong>{{ receipt.chargerCode }}</strong></li>
          <li><span>时长</span><strong data-testid="receipt-duration">{{ formatDuration(receipt.durationSec) }}</strong></li>
          <li><span>电量</span><strong data-testid="receipt-energy">{{ formatEnergy(receipt.energyMwh) }}</strong></li>
          <li><span>应付</span><strong data-testid="receipt-amount">{{ formatYuan(receipt.amountCent) }} 元</strong></li>
          <li><span>实付</span><strong>{{ formatYuan(receipt.paidCent) }} 元</strong></li>
          <li v-if="receipt.paymentStatus === 'PARTIAL_PAID'"><span>支付状态</span><strong>部分支付（余额不足部分待结清）</strong></li>
        </ul>
        <div class="panel__row">
          <RouterLink class="btn btn--primary" to="/orders">查看订单</RouterLink>
          <button type="button" class="btn" @click="goHome">返回首页</button>
        </div>
      </div>

      <div v-else-if="isActive" v-reveal class="panel" data-testid="charging-flow">
        <header class="panel__row">
          <h2 data-testid="charging-status">{{ order.statusText || order.status }}</h2>
          <span class="muted" data-testid="charging-flow-no">{{ order.orderNo }}</span>
        </header>

        <p v-if="order.chargerCode" data-testid="charging-charger">设备 {{ order.chargerCode }}</p>

        <template v-if="status === 'CREATED'">
          <p>订单已创建并锁定设备，请尽快开始充电。</p>
          <div class="panel__row">
            <button type="button" class="btn btn--primary" data-testid="charging-start" :disabled="busy" @click="runAction('start')">开始充电</button>
            <button type="button" class="btn" data-testid="charging-cancel" :disabled="busy" @click="runAction('cancel')">取消订单</button>
          </div>
        </template>

        <template v-else-if="status === 'STARTING'">
          <p data-testid="charging-starting">启动命令已提交，等待设备回执确认…</p>
        </template>

        <template v-else-if="status === 'CHARGING'">
          <div class="charge-energy" aria-hidden="true">
            <span>已充电量</span>
            <strong>{{ formatEnergy(energyMwh) }}</strong>
          </div>
          <ul class="metric-list" data-testid="charging-progress">
            <li>
              <span>已充电量</span>
              <strong class="value-target" :class="{ 'value-flash': energyFlashing }" data-testid="progress-energy">{{ formatEnergy(energyMwh) }}</strong>
            </li>
            <li>
              <span>已充金额</span>
              <strong class="value-target" :class="{ 'value-flash': amountFlashing }" data-testid="progress-amount">{{ formatYuan(amountCent) }} 元</strong>
            </li>
            <li><span>时长</span><strong data-testid="progress-duration">{{ formatDuration(durationSec) }}</strong></li>
          </ul>
          <button type="button" class="btn btn--primary" data-testid="charging-stop" :disabled="busy" @click="runAction('stop')">停止充电</button>
        </template>

        <template v-else-if="status === 'STOPPING'">
          <p data-testid="charging-stopping">停止命令已提交，等待设备回执并结算…</p>
        </template>

        <template v-else>
          <p class="muted" data-testid="charging-other-state">当前状态：{{ order.statusText || order.status }}，可刷新获取最新状态。</p>
          <button type="button" class="btn" data-testid="charging-refresh" @click="refreshOrder">刷新</button>
        </template>
      </div>

      <div v-else-if="!loading" class="panel" data-testid="charging-idle">
        <p>当前没有进行中的充电订单。</p>
        <RouterLink class="btn btn--primary" to="/" data-testid="charging-find-station">去找充电站</RouterLink>
      </div>
    </template>
  </section>
</template>

<style scoped>
.wallet-bar {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 12px;
}
</style>
