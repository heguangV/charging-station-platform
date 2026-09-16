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
const appealDone = ref(false)
const stationNames = ref({})

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
  appealDone.value = false
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
    await orderApi.createAppeal(selectedOrderNo.value, { reason }, pendingAppealKey)
    pendingAppealKey = null
    appealDone.value = true
  } catch (caught) {
    if (caught?.status === 409) {
      appealDone.value = true
      appealError.value = ''
      pendingAppealKey = null
    } else {
      appealError.value = caught?.userMessage || '申诉提交失败，请稍后重试'
    }
  } finally {
    appealBusy.value = false
  }
}

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

        <div class="review-box" data-testid="order-review">
          <h3>订单评价</h3>

          <div v-if="review" data-testid="order-review-existing">
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

        <div v-if="receipt.status === 'COMPLETED'" class="review-box" data-testid="order-appeal">
          <h3>订单申诉</h3>

          <div v-if="appealDone" data-testid="order-appeal-done">
            <p>申诉已提交，客服审核通过后会把实付金额退回钱包。</p>
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
