<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import * as chargingApi from '@/api/charging'
import { randomId } from '@/api/http'
import { formatDuration, formatEnergy, formatYuan } from '@/services/coordinate'
import AppSkeleton from '@/components/AppSkeleton.vue'
import { useValueFlash } from '@/composables/useValueFlash'
import { useAuthStore } from '@/stores/auth'

/** 充电流程：排队/待确认报价/已预约/充电中/结算，进度由服务端快照计算，客户端只轮询展示。 */
const route = useRoute()
const router = useRouter()
const auth = useAuthStore()

const flow = ref(null)
const progress = ref(null)
const receipt = ref(null)
const loading = ref(false)
const busy = ref(false)
const error = ref('')
const actionError = ref('')
const rechargeAmountYuan = ref('50')

const POLL_INTERVAL_MS = 3000
let pollTimer = null
let pendingRechargeKey = null

const status = computed(() => flow.value?.status ?? null)
const quote = computed(() => flow.value?.quote || null)
const isActive = computed(() => flow.value !== null && ![60, 70, 90].includes(status.value))
const socText = computed(() => (Number.isInteger(progress.value?.simulatedSoc) ? `${progress.value.simulatedSoc}%` : '--'))

/**
 * 实时进度每几秒轮询一次，数值变化时用一次短暂高亮提示“数据刚更新”。
 * 只切换 class，不参与任何格式化，因此断言到的文本始终是最终值。
 */
const { flashing: energyFlashing } = useValueFlash(() => progress.value?.energyMwh ?? 0)
const { flashing: amountFlashing } = useValueFlash(() => progress.value?.amountCent ?? 0)
const { flashing: balanceFlashing } = useValueFlash(() => auth.balanceCent)
const { flashing: socFlashing } = useValueFlash(() => progress.value?.simulatedSoc ?? null)

onMounted(async () => {
  if (!auth.isLoggedIn) return
  await auth.refreshWallet()
  await refreshFlow()
  const stationId = Number(route.query.stationId)
  const chargerType = Number(route.query.chargerType)
  if (!flow.value && Number.isInteger(stationId) && stationId > 0) {
    await requestNewFlow(stationId, chargerType === 0 ? 0 : 1)
  }
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
    if (flow.value?.flowNo) void refreshProgress()
  }, POLL_INTERVAL_MS)
}

async function refreshFlow() {
  if (!auth.isLoggedIn) return
  loading.value = true
  error.value = ''
  try {
    const active = await chargingApi.fetchActiveFlow()
    if (!active?.hasActiveFlow || !active.flow) {
      flow.value = null
      progress.value = null
      stopPolling()
      return
    }
    flow.value = await chargingApi.fetchFlow(active.flow.flowNo)
    if (flow.value.status === 40) {
      await refreshProgress()
      startPolling()
    } else {
      stopPolling()
      progress.value = null
    }
  } catch (caught) {
    error.value = caught?.userMessage || '充电状态加载失败，请稍后重试'
  } finally {
    loading.value = false
  }
}

async function refreshProgress() {
  if (!flow.value?.flowNo) return
  try {
    progress.value = await chargingApi.fetchProgress(flow.value.flowNo)
  } catch (caught) {
    actionError.value = caught?.userMessage || '进度刷新失败'
  }
}

async function requestNewFlow(stationId, chargerType) {
  busy.value = true
  actionError.value = ''
  try {
    await chargingApi.requestFlow({ stationId, chargerType }, randomId())
    await refreshFlow()
  } catch (caught) {
    actionError.value = caught?.userMessage || '发起充电失败，请稍后重试'
  } finally {
    busy.value = false
  }
}

async function runAction(action) {
  if (!flow.value) return
  busy.value = true
  actionError.value = ''
  try {
    const flowNo = flow.value.flowNo
    const version = flow.value.version
    if (action === 'confirm') {
      await chargingApi.confirmQuote(flowNo, { quoteNo: quote.value?.quoteNo, flowVersion: version }, randomId())
    } else if (action === 'cancel') {
      await chargingApi.cancelFlow(flowNo, { reasonCode: 'USER_CANCELLED', flowVersion: version }, randomId())
    } else if (action === 'start') {
      await chargingApi.startFlow(flowNo, { flowVersion: version }, randomId())
    } else if (action === 'settle') {
      receipt.value = await chargingApi.settleFlow(flowNo, { flowVersion: version, reasonCode: 'USER_STOPPED' }, randomId())
      stopPolling()
      flow.value = null
      progress.value = null
      await auth.refreshWallet()
      return
    }
    await refreshFlow()
  } catch (caught) {
    actionError.value = caught?.userMessage || '操作失败，请稍后重试'
  } finally {
    busy.value = false
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
        <p v-if="auth.hasDebt" class="alert alert--error" data-testid="charging-debt">存在欠费 {{ formatYuan(auth.debtCent) }} 元，请先充值清偿。</p>
        <div class="inline-form">
          <input v-model="rechargeAmountYuan" data-testid="charging-recharge-amount" type="number" min="0.01" step="0.01" />
          <button type="button" class="btn" data-testid="charging-recharge" :disabled="busy" @click="recharge">充值</button>
        </div>
      </div>

      <p v-if="error" class="alert alert--error" data-testid="charging-error" role="alert">
        {{ error }}
        <button type="button" class="btn" data-testid="charging-retry" @click="refreshFlow">重试</button>
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
          <li v-if="receipt.debtAddedCent"><span>新增欠费</span><strong>{{ formatYuan(receipt.debtAddedCent) }} 元</strong></li>
        </ul>
        <div class="panel__row">
          <RouterLink class="btn btn--primary" to="/orders">查看订单</RouterLink>
          <button type="button" class="btn" @click="goHome">返回首页</button>
        </div>
      </div>

      <div v-else-if="isActive" v-reveal class="panel" data-testid="charging-flow">
        <header class="panel__row">
          <h2 data-testid="charging-status">{{ flow.statusText || flow.status }}</h2>
          <span class="muted" data-testid="charging-flow-no">{{ flow.flowNo }}</span>
        </header>

        <p v-if="flow.chargerCode" data-testid="charging-charger">设备 {{ flow.chargerCode }}</p>

        <template v-if="status === 10">
          <p data-testid="charging-queue">排队中，前面还有 {{ flow.queuePosition ?? '—' }} 位。</p>
          <button type="button" class="btn" data-testid="charging-cancel" :disabled="busy" @click="runAction('cancel')">取消排队</button>
        </template>

        <template v-else-if="status === 20 && quote">
          <ul class="metric-list" data-testid="charging-quote">
            <li><span>电费</span><strong>{{ formatYuan(quote.electricityPriceCentPerKwh) }} 元/kWh</strong></li>
            <li><span>服务费</span><strong>{{ formatYuan(quote.finalServicePriceCentPerKwh) }} 元/kWh</strong></li>
            <li><span>合计</span><strong data-testid="charging-quote-total">{{ formatYuan(quote.totalPriceCentPerKwh) }} 元/kWh</strong></li>
          </ul>
          <div class="panel__row">
            <button type="button" class="btn btn--primary" data-testid="charging-confirm" :disabled="busy" @click="runAction('confirm')">确认报价并预约</button>
            <button type="button" class="btn" data-testid="charging-cancel" :disabled="busy" @click="runAction('cancel')">取消</button>
          </div>
        </template>

        <template v-else-if="status === 30">
          <p>已预约设备，请尽快到达并开始充电。</p>
          <div class="panel__row">
            <button type="button" class="btn btn--primary" data-testid="charging-start" :disabled="busy" @click="runAction('start')">开始充电</button>
            <button type="button" class="btn" data-testid="charging-cancel" :disabled="busy" @click="runAction('cancel')">取消预约</button>
          </div>
        </template>

        <template v-else-if="status === 40">
          <div class="charge-energy" aria-hidden="true">
            <span>已充电量</span>
            <strong>{{ formatEnergy(progress?.energyMwh ?? 0) }}</strong>
          </div>
          <ul class="metric-list" data-testid="charging-progress">
            <li>
              <span>已充电量</span>
              <strong class="value-target" :class="{ 'value-flash': energyFlashing }" data-testid="progress-energy">{{ formatEnergy(progress?.energyMwh ?? 0) }}</strong>
            </li>
            <li>
              <span>已充金额</span>
              <strong class="value-target" :class="{ 'value-flash': amountFlashing }" data-testid="progress-amount">{{ formatYuan(progress?.amountCent ?? 0) }} 元</strong>
            </li>
            <li><span>时长</span><strong data-testid="progress-duration">{{ formatDuration(progress?.durationSec ?? 0) }}</strong></li>
            <li><span>功率</span><strong>{{ ((progress?.powerWatt ?? 0) / 1000).toFixed(1) }} kW</strong></li>
            <li>
              <span>模拟 SOC</span>
              <strong class="value-target" :class="{ 'value-flash': socFlashing }" data-testid="progress-soc">{{ socText }}</strong>
            </li>
          </ul>
          <button type="button" class="btn btn--primary" data-testid="charging-settle" :disabled="busy" @click="runAction('settle')">结束充电并结算</button>
        </template>

        <template v-else>
          <p class="muted" data-testid="charging-other-state">当前状态：{{ flow.statusText || flow.status }}，可刷新获取最新状态。</p>
          <button type="button" class="btn" data-testid="charging-refresh" @click="refreshFlow">刷新</button>
        </template>
      </div>

      <div v-else-if="!loading" class="panel" data-testid="charging-idle">
        <p>当前没有进行中的充电流程。</p>
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
