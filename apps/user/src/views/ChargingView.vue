<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import * as chargingApi from '@/api/charging'
import * as orderApi from '@/api/order'
import { ApiError } from '@/api/http'
import { randomId } from '@/api/http'
import { formatDuration, formatEnergy, formatYuan } from '@/services/coordinate'
import AppSkeleton from '@/components/AppSkeleton.vue'
import { useValueFlash } from '@/composables/useValueFlash'
import { useAuthStore } from '@/stores/auth'

/**
 * 充电页（Go 订单模型）：CREATED 是 15 分钟设备预约，用户明确确认后才发送
 * START；START/STOP 是 202 异步设备命令，结算由设备回执驱动。前端只轮询订单详情展示状态与累计量，
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

/**
 * 订单详情比列表多带的字段，单独存放，**不要并进 order**：活动订单每 3 秒由列表轮询整体覆盖，
 * 而列表没有这些字段，合并进去的值会在下一次轮询被抹掉（实测：预估出现 12 秒后消失）。
 *
 * - estimateBasis：首读到达前的估算依据（startedAt / chargerPowerWatt / unitPriceCentPerKwh）
 * - metered：设备周期上报的实时计量（已充电量 / 金额 / 读数时间），优先显示
 */
const estimateBasis = ref(null)
const metered = ref(null)

const POLL_INTERVAL_MS = 3000
let basisOrderNo = ''
let pollTimer = null
let pendingRechargeKey = null

const status = computed(() => order.value?.status ?? '')
const isActive = computed(() => order.value !== null && order.value.active === true)
const reservationRemainingSec = computed(() => {
  if (status.value !== 'CREATED' || !Number.isFinite(order.value?.reservedUntil)) return 0
  return Math.max(0, order.value.reservedUntil - nowSecond.value)
})
const reservationExpired = computed(() => status.value === 'CREATED' && reservationRemainingSec.value <= 0)
const reservationCountdown = computed(() => {
  const seconds = reservationRemainingSec.value
  const minutes = Math.floor(seconds / 60)
  return `${String(minutes).padStart(2, '0')}:${String(seconds % 60).padStart(2, '0')}`
})
/**
 * 充电中的实时计量：设备通过 CHARGE_PROGRESS 周期上报读数，平台把它换算成
 * `meteredEnergyWh`（已充电量）与 `meteredAmountCent`（这份电量在订单冻结快照下的金额，
 * 用的是与最终账单**同一个分时引擎**）。它们与结算用的 energyWh/amountCent 分开，
 * 因此这里优先显示实时值，结算后再由小票给出最终数字。
 *
 * 把"还没有读数"与"读数是 0"分开：首读到达前显示占位符或预估，而不是假的 0.00。
 */
const meteredEnergyMwh = computed(() => metered.value?.energyMwh ?? null)

const meteredAmountCent = computed(() => metered.value?.amountCent ?? null)

const hasMetering = computed(() => meteredEnergyMwh.value !== null && meteredAmountCent.value !== null)

/** 实时读数的设备事实时间（HH:mm:ss），让用户知道数字有多新。 */
const meteredAtText = computed(() => {
  const seconds = metered.value?.at
  if (!Number.isFinite(seconds) || seconds <= 0) return ''
  const date = new Date(seconds * 1000)
  const pad = value => String(value).padStart(2, '0')
  return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`
})

/**
 * 首读到达前的预估：设备通常几秒内就会报第一次计量，这段空窗期按
 * 「额定功率 × 已充时长」估算，单价取订单详情里服务端解析好的当前时段单价
 * （chargerPowerWatt / unitPriceCentPerKwh 是契约里只读的估算依据字段）。
 *
 * 三个输入缺任何一个都返回 null —— 宁可显示占位符，也不编造数字。
 */
const estimate = computed(() => {
  const powerWatt = estimateBasis.value?.chargerPowerWatt
  const unitPriceCentPerKwh = estimateBasis.value?.unitPriceCentPerKwh
  const startedAt = estimateBasis.value?.startedAt
  if (!Number.isFinite(powerWatt) || !Number.isFinite(unitPriceCentPerKwh)) return null
  if (!Number.isFinite(startedAt) || startedAt <= 0) return null
  const elapsedSec = Math.max(0, nowSecond.value - startedAt)
  const energyWh = (powerWatt * elapsedSec) / 3600
  return {
    energyMwh: energyWh * 1000,
    amountCent: (energyWh * unitPriceCentPerKwh) / 1000,
    powerKw: powerWatt / 1000
  }
})

/** 充电中显示什么：设备实时读数 > 首读到达前的预估 > 占位符。 */
const energyText = computed(() => {
  if (hasMetering.value) return formatEnergy(meteredEnergyMwh.value)
  return estimate.value ? `预估 ${formatEnergy(estimate.value.energyMwh)}` : '—'
})

const amountText = computed(() => {
  if (hasMetering.value) return `${formatYuan(meteredAmountCent.value)} 元`
  return estimate.value ? `预估 ${formatYuan(estimate.value.amountCent)} 元` : '—'
})

/**
 * 充电时长：本地时钟实时走。
 *
 * 不能用订单行的 updatedAt - createdAt —— 充电中订单行不再更新（没有计量就没有写库），
 * 那个差值会一直冻在状态切换那一刻（实测是"4 秒"），看起来像卡死。
 * 契约没有 startedAt，适配层用 createdAt 兜底（下单到进入 CHARGING 通常只隔几秒）。
 */
const nowSecond = ref(Math.floor(Date.now() / 1000))
const durationSec = computed(() => {
  // 优先用设备事实时间（详情才有），列表轮询拿不到时退回 createdAt 兜底。
  const startedAt = estimateBasis.value?.startedAt ?? order.value?.startedAt
  if (!Number.isFinite(startedAt) || startedAt <= 0) return 0
  return Math.max(0, nowSecond.value - startedAt)
})

let tickTimer = null

function startTicking() {
  nowSecond.value = Math.floor(Date.now() / 1000)
  if (tickTimer) return
  tickTimer = setInterval(() => {
    nowSecond.value = Math.floor(Date.now() / 1000)
  }, 1000)
}

function stopTicking() {
  if (tickTimer) {
    clearInterval(tickTimer)
    tickTimer = null
  }
}

/**
 * 充电量/金额变化时用一次短暂高亮提示“数据刚更新”。
 * 只切换 class，不参与任何格式化，因此断言到的文本始终是最终值。
 */
const { flashing: energyFlashing } = useValueFlash(() => metered.value?.energyMwh ?? estimate.value?.energyMwh ?? 0)
const { flashing: amountFlashing } = useValueFlash(() => metered.value?.amountCent ?? estimate.value?.amountCent ?? 0)
const { flashing: balanceFlashing } = useValueFlash(() => auth.balanceCent)

onMounted(async () => {
  if (!auth.isLoggedIn) return
  await auth.refreshWallet()
  await refreshOrder()
})

onBeforeUnmount(() => {
  stopPolling()
  stopTicking()
})

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

/**
 * 取回订单详情：实时计量与估算依据都在里面（列表不带这些字段）。
 *
 * 充电中每次轮询都取一次，实时值才跟得上设备的上报节奏（网关默认每几秒一条
 * CHARGE_PROGRESS）；拿不到就地放弃，页面退回预估或占位符，绝不编造数字。
 */
async function refreshDetail() {
  const active = order.value
  if (!active?.orderNo || !active.active) return
  if (active.orderNo !== basisOrderNo) {
    basisOrderNo = active.orderNo
    estimateBasis.value = null
    metered.value = null
  }
  try {
    const detail = await orderApi.fetchOrderDetail(active.orderNo)
    if (order.value?.orderNo !== active.orderNo) return
    estimateBasis.value = {
      startedAt: detail.startedAt,
      chargerPowerWatt: detail.chargerPowerWatt,
      unitPriceCentPerKwh: detail.unitPriceCentPerKwh
    }
    metered.value =
      Number.isFinite(detail.meteredEnergyMwh) && Number.isFinite(detail.meteredAmountCent)
        ? { energyMwh: detail.meteredEnergyMwh, amountCent: detail.meteredAmountCent, at: detail.meteredAt }
        : null
  } catch (error) {
    // 接口不可用时放弃这两个数据源（旧服务端没有这些字段、网络失败）。但代码层面的错误必须
    // 原样抛出：这个 catch 曾经吞掉过一次"接口函数名写错"的 TypeError，页面只表现为没有预估。
    if (!(error instanceof ApiError)) throw error
  }
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
      // STARTING/STOPPING 阶段等待设备回执；CHARGING 阶段跟踪订单状态与时长。
      startPolling()
      startTicking()
      void refreshDetail()
    } else {
      stopPolling()
      stopTicking()
      estimateBasis.value = null
      metered.value = null
      basisOrderNo = ''
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
    actionError.value = caught?.code === 17 ? '预约已过期，设备已经释放，请重新预约' : caught?.userMessage || '操作失败，请稍后重试'
    if (caught?.code === 17) await refreshOrder()
  } finally {
    busy.value = false
  }
}

/** 待确认的结算单（UC-U-09）：不确认会阻止下一个充电流程，按钮必须可见。 */
const receiptUnsettled = computed(() => receipt.value?.paymentStatus === 'PENDING')

/** 小票：订单离开活动集合且终态为 COMPLETED 时展示；其余终态直接回空闲页。 */
async function loadReceipt(orderNo) {
  // 小票属于订单接口组（@/api/order，与"我的订单"页同一入口），不在 @/api/charging 中。
  const detail = await orderApi.fetchOrderReceipt(orderNo)
  stopPolling()
  stopTicking()
  order.value = null
  if (detail?.status === 'COMPLETED') {
    receipt.value = detail
    await auth.refreshWallet()
    await auth.refreshProfile()
  }
}

async function confirmPayment() {
  if (!receipt.value) return
  busy.value = true
  actionError.value = ''
  try {
    // 超时重试复用同一幂等键由 http 层处理；确认本身幂等，重复提交返回首次结果。
    const updated = await orderApi.confirmOrder(receipt.value.orderNo, randomId())
    receipt.value = updated
    await auth.refreshWallet()
    await auth.refreshProfile()
  } catch (caught) {
    actionError.value = caught?.userMessage || '确认支付失败，请稍后重试'
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
          <li>
            <span>支付状态</span>
            <strong data-testid="receipt-payment">
              {{ receipt.paymentStatus === 'PARTIAL_PAID' ? '部分支付（余额不足部分待结清）' : receipt.paymentStatus === 'PAID' ? '已支付' : '待确认' }}
            </strong>
          </li>
        </ul>
        <p v-if="receiptUnsettled" class="muted" data-testid="receipt-confirm-hint">
          确认后从余额扣款；未确认的订单会阻止发起下一次充电。
        </p>
        <div class="panel__row">
          <button
            v-if="receiptUnsettled"
            type="button"
            class="btn btn--primary"
            data-testid="receipt-confirm-payment"
            :disabled="busy"
            @click="confirmPayment"
          >
            {{ busy ? '确认中…' : '确认支付' }}
          </button>
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
          <p>设备已为你保留 15 分钟。到达充电桩并确认连接后，再开始充电。</p>
          <p v-if="!reservationExpired" class="reservation-countdown" data-testid="reservation-countdown">
            剩余预约时间 <strong>{{ reservationCountdown }}</strong>
          </p>
          <p v-else class="alert alert--error" data-testid="reservation-expired">
            预约时间已结束，请释放本次预约后重新选择设备。
          </p>
          <div class="panel__row">
            <button type="button" class="btn btn--primary" data-testid="charging-start" :disabled="busy || reservationExpired" @click="runAction('start')">开始充电</button>
            <button type="button" class="btn" data-testid="charging-cancel" :disabled="busy" @click="runAction('cancel')">取消预约</button>
          </div>
        </template>

        <template v-else-if="status === 'STARTING'">
          <p data-testid="charging-starting">启动命令已提交，等待设备回执确认…</p>
        </template>

        <template v-else-if="status === 'CHARGING'">
          <div class="charge-energy" aria-hidden="true">
            <span>已充电量</span>
            <strong>{{ energyText }}</strong>
          </div>
          <ul class="metric-list" data-testid="charging-progress">
            <li>
              <span>已充电量</span>
              <strong class="value-target" :class="{ 'value-flash': energyFlashing }" data-testid="progress-energy">{{ energyText }}</strong>
            </li>
            <li>
              <span>已充金额</span>
              <strong class="value-target" :class="{ 'value-flash': amountFlashing }" data-testid="progress-amount">{{ amountText }}</strong>
            </li>
            <li><span>时长</span><strong data-testid="progress-duration">{{ formatDuration(durationSec) }}</strong></li>
          </ul>
          <p v-if="hasMetering" class="muted" data-testid="progress-metering-live">
            实时计量<template v-if="meteredAtText">（设备 {{ meteredAtText }} 读数）</template>；停止充电后的结算小票为最终金额。
          </p>
          <p v-else class="muted" data-testid="progress-metering-hint">
            <template v-if="estimate">
              设备还没上报第一次计量，暂按额定功率 {{ estimate.powerKw }} kW × 已充时长预估；读数到达后会自动换成实时值。
            </template>
            <template v-else>
              等待设备上报计量读数；读数到达前暂不显示电量与金额。
            </template>
          </p>
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

.reservation-countdown {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: var(--ncs-s-3);
  max-width: 360px;
}

.reservation-countdown strong {
  min-width: 5ch;
  font-variant-numeric: tabular-nums;
  color: var(--ncs-brand-strong);
}
</style>
