<script setup>
import { computed, onMounted, ref } from 'vue'
import * as orderApi from '@/api/order'
import * as stationApi from '@/api/station'
import { randomId } from '@/api/http'
import { formatDateTime, formatEnergy, formatDuration, formatYuan } from '@/services/coordinate'
import AppSkeleton from '@/components/AppSkeleton.vue'
import { staggerStyle } from '@/composables/useReveal'
import { useAuthStore } from '@/stores/auth'

/**
 * 我的订单：分页列表 + 订单小票 + 评价与申诉。
 * 评价每单仅一次（同内容重放返回首次结果），失败重试复用幂等键；
 * 申诉针对已完成的订单，重复申诉（409）按已提交提示。
 */
const auth = useAuthStore()

const orders = ref([])
const total = ref(0)
const page = ref(1)
const pageSize = 10
const statusFilter = ref('')
const loading = ref(false)
const error = ref('')
const receipt = ref(null)
const selectedOrderNo = ref('')
const review = ref(null)
const rating = ref(5)
const content = ref('')
const reviewError = ref('')
const reviewBusy = ref(false)
const appealReason = ref('')
const appealError = ref('')
const appealBusy = ref(false)
/** 该订单已存在但状态未知的申诉（409 后重新查询也失败时的兜底）。 */
const appealExists = ref(false)
/** 409：该订单已有申诉，本次填写的内容没有被保存。 */
const appealConflict = ref(false)
/**
 * 该订单已存在的申诉（PENDING/APPROVED/REJECTED）。
 * 契约给了本人查询端点，所以刷新页面后也能显示"审核中/已通过/已驳回"，
 * 而不是又把申诉表单摆出来——以前用户投过一次却看不到任何结果，只能反复重投。
 */
const existingAppeal = ref(null)
const stationNames = ref({})

const confirmingOrderNo = ref('')
const confirmError = ref('')

let pendingReviewKey = null
let pendingAppealKey = null

const totalPages = computed(() => Math.max(1, Math.ceil(total.value / pageSize)))
const statusOptions = [
  { label: '全部', value: '' },
  { label: '已完成', value: 'COMPLETED' },
  { label: '已取消', value: 'CANCELLED' },
  { label: '已失败', value: 'FAILED' },
  { label: '已过期', value: 'EXPIRED' }
]

onMounted(async () => {
  if (!auth.isLoggedIn) return
  await loadStationNames()
  await loadOrders(1)
})

/**
 * 确认支付（UC-U-09）：COMPLETED 且 PENDING 的订单从余额扣款；未确认订单会
 * 阻止新的充电流程，因此列表里必须能直接确认。余额不足时服务端扣至零并记录
 * 欠费（PARTIAL_PAID），属最终状态，不再提供二次确认。
 */
async function confirmPayment(order) {
  if (order.status !== 'COMPLETED' || order.paymentStatus !== 'PENDING') return
  confirmingOrderNo.value = order.orderNo
  confirmError.value = ''
  try {
    const updated = await orderApi.confirmOrder(order.orderNo, randomId())
    const index = orders.value.findIndex(item => item.orderNo === order.orderNo)
    if (index >= 0) orders.value[index] = updated
    await auth.refreshWallet()
  } catch (caught) {
    confirmError.value = caught?.userMessage || '确认支付失败，请稍后重试'
  } finally {
    confirmingOrderNo.value = ''
  }
}

/** 订单列表只带 stationId；拉一次站点列表把 id 解析成名称，失败时回退“站点 #id”。 */
async function loadStationNames() {
  try {
    const data = await stationApi.fetchStations({ page: 1, pageSize: 100 })
    const map = {}
    for (const item of data.items || []) map[item.id] = item.name
    stationNames.value = map
  } catch {
    stationNames.value = {}
  }
}

function withStationName(order) {
  return { ...order, stationName: stationNames.value[order.stationId] || order.stationName }
}

async function loadOrders(targetPage = page.value) {
  loading.value = true
  error.value = ''
  try {
    const data = await orderApi.fetchOrders({
      status: statusFilter.value === '' ? undefined : statusFilter.value,
      page: targetPage,
      pageSize
    })
    orders.value = (Array.isArray(data.items) ? data.items : []).map(withStationName)
    total.value = Number.isInteger(data.total) ? data.total : orders.value.length
    page.value = Number.isInteger(data.page) ? data.page : targetPage
  } catch (caught) {
    orders.value = []
    error.value = caught?.userMessage || '订单加载失败，请稍后重试'
  } finally {
    loading.value = false
  }
}

async function openOrder(orderNo) {
  selectedOrderNo.value = orderNo
  receipt.value = null
  review.value = null
  reviewError.value = ''
  appealError.value = ''
  appealExists.value = false
  appealConflict.value = false
  existingAppeal.value = null
  pendingReviewKey = null
  pendingAppealKey = null
  content.value = ''
  rating.value = 5
  try {
    receipt.value = withStationName(await orderApi.fetchOrderReceipt(orderNo))
  } catch (caught) {
    reviewError.value = caught?.userMessage || '订单小票加载失败'
  }
  try {
    existingAppeal.value = await orderApi.fetchOrderAppeal(orderNo)
    if (existingAppeal.value) appealExists.value = true
  } catch (caught) {
    // 申诉状态拿不到不影响小票与评价；表单仍可用，提交时服务端会给出准确结论。
    existingAppeal.value = null
    void caught
  }
  try {
    review.value = await orderApi.fetchOrderReview(orderNo)
    return
  } catch (caught) {
    // Go 契约：未评价返回 404，等同于旧契约的 {review: null}。
    if (caught?.status !== 404) reviewError.value = caught?.userMessage || '评价查询失败'
  }
}

async function submitReview() {
  const text = content.value.trim()
  if (!Number.isInteger(rating.value) || rating.value < 1 || rating.value > 5) {
    reviewError.value = '请选择 1~5 星评分'
    return
  }
  if (!text) {
    reviewError.value = '请填写评价内容'
    return
  }
  reviewBusy.value = true
  reviewError.value = ''
  pendingReviewKey = pendingReviewKey || randomId()
  try {
    review.value = await orderApi.submitOrderReview(selectedOrderNo.value, { rating: rating.value, content: text }, pendingReviewKey)
    pendingReviewKey = null
    content.value = ''
  } catch (caught) {
    reviewError.value = caught?.userMessage || '评价提交失败，请稍后重试'
  } finally {
    reviewBusy.value = false
  }
}

/** 提交申诉：仅本人已完成订单；同内容重放返回首次结果，不同内容冲突。 */
async function submitAppeal() {
  const reason = appealReason.value.trim()
  if (!reason) {
    appealError.value = '请填写申诉原因'
    return
  }
  appealBusy.value = true
  appealError.value = ''
  pendingAppealKey = pendingAppealKey || randomId()
  try {
    const created = await orderApi.createAppeal(selectedOrderNo.value, { reason }, pendingAppealKey)
    pendingAppealKey = null
    existingAppeal.value = created ? { ...created, status: created.status || 'PENDING' } : { status: 'PENDING' }
  } catch (caught) {
    if (caught?.status === 409) {
      // 契约：同一订单只能有一条申诉，内容不同返回 409。
      // 以前这里直接当成"提交成功"，用户会以为这次填的内容也被受理了。
      pendingAppealKey = null
      appealConflict.value = true
      appealError.value = ''
      try {
        existingAppeal.value = await orderApi.fetchOrderAppeal(selectedOrderNo.value)
      } catch {
        existingAppeal.value = null
      }
      if (!existingAppeal.value) appealExists.value = true
    } else {
      appealError.value = caught?.userMessage || '申诉提交失败，请稍后重试'
    }
  } finally {
    appealBusy.value = false
  }
}

/** 本人申诉状态文案；PENDING/APPROVED 属于"仍在生效"（会阻止评价）。 */
const appealStatusText = status => {
  if (status === 'PENDING') return '审核中'
  if (status === 'APPROVED') return '已通过（实付金额已退回钱包）'
  if (status === 'REJECTED') return '已驳回'
  return status || '—'
}

/** 生效中的申诉会阻止评价（契约：有申诉的订单不能再评价）。 */
const liveAppeal = computed(() => existingAppeal.value !== null && existingAppeal.value.status !== 'REJECTED')

function changeStatus(value) {
  statusFilter.value = value
  void loadOrders(1)
}
</script>

<template>
  <section class="view orders-view" data-testid="orders-view">
    <div v-if="!auth.isLoggedIn" class="panel" data-testid="orders-login-required">
      <p>登录后可以查看你的充电订单与评价。</p>
      <RouterLink class="btn btn--primary" to="/profile">去登录</RouterLink>
    </div>

    <template v-else>
      <div class="panel panel__row panel__row--wrap">
        <button
          v-for="option in statusOptions"
          :key="option.value"
          type="button"
          class="chip"
          :class="{ 'is-active': statusFilter === option.value }"
          :data-testid="`orders-status-${option.value || 'all'}`"
          @click="changeStatus(option.value)"
        >
          {{ option.label }}
        </button>
        <button type="button" class="btn" data-testid="orders-refresh" @click="loadOrders()">刷新</button>
      </div>

      <div v-if="loading" data-testid="orders-loading" role="status">
        <span class="sr-only">正在加载订单…</span>
        <AppSkeleton variant="cards" :rows="3" />
      </div>

      <div v-else-if="error" class="alert alert--error" data-testid="orders-error" role="alert">
        <p>{{ error }}</p>
        <button type="button" class="btn btn--primary" data-testid="orders-retry" @click="loadOrders()">重试</button>
      </div>

      <p v-else-if="!orders.length" class="empty-state" data-testid="orders-empty">还没有充电订单，去附近站点开始第一次充电吧。</p>

      <p v-if="confirmError" class="alert alert--error" data-testid="orders-confirm-error" role="alert">{{ confirmError }}</p>

      <TransitionGroup v-else name="list" tag="ul" class="order-list" data-testid="orders-list">
        <li v-for="(order, index) in orders" :key="order.orderNo" class="stagger-item" :style="staggerStyle(index)">
          <button type="button" class="order-list__item" :data-testid="`order-${order.orderNo}`" @click="openOrder(order.orderNo)">
            <span class="order-list__station">{{ order.stationName }}</span>
            <span class="muted">{{ order.chargerCode }}</span>
            <span class="muted" data-testid="order-status">{{ order.statusText }}</span>
            <span class="muted">{{ formatDateTime(order.startedAt) }}</span>
            <span>{{ formatEnergy(order.energyMwh) }}</span>
            <strong>{{ formatYuan(order.amountCent) }} 元</strong>
          </button>
          <div
            v-if="order.status === 'COMPLETED' && order.paymentStatus === 'PENDING'"
            class="order-list__settle"
          >
            <span class="muted">充电已完成，请确认支付后才能发起下一次充电。</span>
            <button
              type="button"
              class="btn btn--sm btn--primary"
              :data-testid="`order-confirm-${order.orderNo}`"
              :disabled="confirmingOrderNo === order.orderNo"
              @click="confirmPayment(order)"
            >
              {{ confirmingOrderNo === order.orderNo ? '确认中…' : '确认支付' }}
            </button>
          </div>
        </li>
      </TransitionGroup>

      <div class="panel__row" v-if="totalPages > 1">
        <button type="button" class="btn" :disabled="page <= 1" data-testid="orders-prev" @click="loadOrders(page - 1)">上一页</button>
        <span class="muted">第 {{ page }} / {{ totalPages }} 页</span>
        <button type="button" class="btn" :disabled="page >= totalPages" data-testid="orders-next" @click="loadOrders(page + 1)">下一页</button>
      </div>

      <div v-if="selectedOrderNo" v-reveal class="panel" data-testid="order-detail">
        <h2>订单 {{ selectedOrderNo }}</h2>
        <ul v-if="receipt" class="metric-list">
          <li><span>站点</span><strong>{{ receipt.stationName }}</strong></li>
          <li><span>设备</span><strong>{{ receipt.chargerCode }}</strong></li>
          <li><span>时长</span><strong>{{ formatDuration(receipt.durationSec) }}</strong></li>
          <li><span>电量</span><strong>{{ formatEnergy(receipt.energyMwh) }}</strong></li>
          <li><span>应付</span><strong>{{ formatYuan(receipt.amountCent) }} 元</strong></li>
          <li><span>实付</span><strong>{{ formatYuan(receipt.paidCent) }} 元</strong></li>
          <li><span>结算时间</span><strong>{{ formatDateTime(receipt.settledAt) }}</strong></li>
        </ul>

        <!--
          评价与申诉都只对 COMPLETED 订单开放（契约原文），而且"有申诉的订单不能再评价"。
          receipt 为 null（详情还没回来或加载失败）时不能读它的字段——直接读会让整页渲染抛错。
        -->
        <div v-if="receipt && receipt.status === 'COMPLETED'" class="review-box" data-testid="order-review">
          <h3>订单评价</h3>

          <!-- 契约：有申诉的订单不能再评价。与其让用户填完再吃一个错误，不如说清楚。 -->
          <div v-if="liveAppeal && !review" data-testid="order-review-blocked">
            <p>该订单有生效中的申诉（{{ appealStatusText(existingAppeal.status) }}），申诉处理完之前不能评价。</p>
          </div>

          <div v-else-if="review" data-testid="order-review-existing">
            <p>{{ '★'.repeat(review.rating) }} {{ review.content }}</p>
            <p class="muted">{{ formatDateTime(review.createdAt) }}</p>
          </div>

          <form v-else class="review-form" @submit.prevent="submitReview">
            <label class="field">
              <span>评分</span>
              <select v-model.number="rating" data-testid="review-rating">
                <option v-for="value in [5, 4, 3, 2, 1]" :key="value" :value="value">{{ value }} 星</option>
              </select>
            </label>
            <textarea v-model="content" data-testid="review-content" rows="3" maxlength="500" placeholder="充电体验如何？"></textarea>
            <button type="submit" class="btn btn--primary" data-testid="review-submit" :disabled="reviewBusy">
              {{ reviewBusy ? '提交中…' : '提交评价' }}
            </button>
          </form>

          <p v-if="reviewError" class="alert alert--error" data-testid="review-error">{{ reviewError }}</p>
        </div>

        <!--
          申诉盒对所有订单开放（只要有申诉或订单已完成）：审核通过会把订单变 CANCELLED，
          若只在 COMPLETED 时渲染，用户就再也看不到"已通过、已退款"，只看到一个莫名其妙的"已取消"。
        -->
        <div
          v-if="existingAppeal || (receipt && receipt.status === 'COMPLETED')"
          class="review-box"
          data-testid="order-appeal"
        >
          <h3>订单申诉</h3>

          <!-- 已有申诉：显示真实状态与处理意见，而不是再摆一遍表单 -->
          <div v-if="existingAppeal" data-testid="order-appeal-state">
            <p>
              申诉状态：<strong data-testid="order-appeal-status">{{ appealStatusText(existingAppeal.status) }}</strong>
            </p>
            <p v-if="existingAppeal.reason" class="muted">申诉内容：{{ existingAppeal.reason }}</p>
            <p v-if="existingAppeal.decisionReason" class="muted" data-testid="order-appeal-decision-reason">
              客服处理意见：{{ existingAppeal.decisionReason }}
            </p>
            <p v-if="appealConflict" data-testid="order-appeal-conflict">
              同一订单只能申诉一次，本次填写的内容没有提交。
            </p>
          </div>

          <div v-else-if="appealExists" data-testid="order-appeal-exists">
            <p>该订单已有申诉记录：同一订单只能申诉一次，本次填写的内容没有提交。</p>
          </div>

          <form v-else class="review-form" @submit.prevent="submitAppeal">
            <label class="field">
              <span>申诉原因（1~500 字）</span>
              <textarea v-model="appealReason" data-testid="appeal-reason" rows="3" maxlength="500" placeholder="请说明申诉原因，例如计量争议"></textarea>
            </label>
            <button type="submit" class="btn" data-testid="appeal-submit" :disabled="appealBusy">
              {{ appealBusy ? '提交中…' : '提交申诉' }}
            </button>
          </form>

          <p v-if="appealError" class="alert alert--error" data-testid="appeal-error">{{ appealError }}</p>
        </div>
      </div>
    </template>
  </section>
</template>

<style scoped>
.order-list {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: var(--ncs-s-2);
  position: relative;
}

/* 订单行：悬停时左侧亮起品牌条，提示“可点开小票” */
.order-list__item {
  position: relative;
  width: 100%;
  display: grid;
  grid-template-columns: 1.4fr 1fr 0.8fr 1.2fr 0.8fr 0.8fr;
  gap: var(--ncs-s-3);
  align-items: center;
  padding: 12px 14px;
  border: 1px solid var(--ncs-line);
  border-radius: var(--ncs-r-md);
  background: var(--ncs-surface);
  text-align: left;
  cursor: pointer;
  font-size: var(--ncs-fs-sm);
  color: var(--ncs-text-2);
  overflow: hidden;
  transition:
    transform var(--ncs-dur-2) var(--ncs-ease-out),
    box-shadow var(--ncs-dur-2) var(--ncs-ease-out),
    border-color var(--ncs-dur-2) var(--ncs-ease-out);
}

.order-list__item::before {
  content: '';
  position: absolute;
  left: 0;
  top: 0;
  bottom: 0;
  width: 3px;
  background: linear-gradient(180deg, var(--ncs-brand-bright), var(--ncs-brand-strong));
  transform: scaleY(0);
  transition: transform var(--ncs-dur-2) var(--ncs-ease-out);
}

/* 待结算行内联操作条：提示 + 确认支付 */
.order-list__settle {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--ncs-s-2);
  margin-top: var(--ncs-s-1);
  padding: 8px 14px;
  border: 1px dashed var(--ncs-line);
  border-radius: var(--ncs-r-sm);
  background: var(--ncs-surface-2);
  font-size: var(--ncs-fs-sm);
}

.order-list__item:hover {
  transform: translate3d(2px, 0, 0);
  box-shadow: var(--ncs-shadow-2);
  border-color: var(--ncs-line-strong);
}

.order-list__item:hover::before {
  transform: scaleY(1);
}

.order-list__item:active {
  transform: translate3d(2px, 0, 0) scale(0.995);
}

.order-list__station {
  font-weight: 650;
  color: var(--ncs-text);
}

.order-list__item strong {
  color: var(--ncs-brand-strong);
  font-weight: 700;
}

.review-box {
  margin-top: var(--ncs-s-4);
  padding-top: var(--ncs-s-3);
  border-top: 1px solid var(--ncs-line);
}

.review-form {
  display: flex;
  flex-direction: column;
  gap: var(--ncs-s-3);
}

@media (max-width: 900px) {
  .order-list__item {
    grid-template-columns: 1fr 1fr;
    row-gap: 4px;
  }
}
</style>
