import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { createMemoryHistory, createRouter } from 'vue-router'
import ChargingView from '../src/views/ChargingView.vue'
import { useAuthStore } from '../src/stores/auth'
import { clearAccessToken, setAccessToken } from '../src/api/http'

/**
 * 充电页在“订单离开活动集合”时会拉取结算小票。
 *
 * 小票入口属于订单接口组（@/api/order），不在 @/api/charging：这里断言真实发出的请求
 * 落在 `/orders/{orderNo}` 上，并渲染出小票。视图此前引用了不存在的
 * `chargingApi.fetchOrderReceipt`，调用点被 refreshOrder 的 try/catch 吞掉，页面只显示
 * “充电状态加载失败”，构建与其它单测都不会报错——这个用例就是为了让那种回归直接失败。
 */

const ORDER_NO = 'ORD20260916000000abcd'

/** Go 契约订单（充电中）。 */
const ACTIVE_ORDER = {
  orderNo: ORDER_NO,
  status: 'CHARGING',
  stationId: 3,
  stationName: 'NCS 中关村充电站',
  chargerId: 28,
  chargerCode: 'A-01',
  energyWh: 1200,
  amountCent: 180,
  createdAt: '2026-09-16T00:00:00Z',
  updatedAt: '2026-09-16T00:20:00Z'
}

/** 同一张订单的终态：结算完成、等待用户确认扣款。 */
const COMPLETED_ORDER = {
  ...ACTIVE_ORDER,
  status: 'COMPLETED',
  paymentStatus: 'PENDING',
  paidCent: 0,
  updatedAt: '2026-09-16T00:30:00Z'
}

function envelope(data) {
  return { success: true, code: 0, message: '', userMessage: '', requestId: 'req-charging', data }
}

function json(data) {
  return { ok: true, status: 200, json: async () => envelope(data) }
}

/**
 * 服务端替身：第一次订单列表返回充电中的订单，之后的列表为空（订单已离开活动集合），
 * 订单详情返回终态小票。
 */
function stubServer() {
  const listCalls = { count: 0 }
  const fetchMock = vi.fn(async (url, init) => {
    void init
    if (url.startsWith('/api/v1/wallet')) return json({ balanceCent: 5000, availableCent: 5000, debtCent: 0 })
    if (url === `/api/v1/orders/${ORDER_NO}`) return json(COMPLETED_ORDER)
    if (url.startsWith('/api/v1/orders')) {
      const first = listCalls.count === 0
      listCalls.count += 1
      return json({
        items: first ? [ACTIVE_ORDER] : [],
        meta: { page: 1, pageSize: 50, total: first ? 1 : 0 }
      })
    }
    if (url.startsWith('/api/v1/me')) return json({ id: 1, displayName: '车主', balanceCent: 5000, debtCent: 0 })
    return json({})
  })
  vi.stubGlobal('fetch', fetchMock)
  return fetchMock
}

async function mountView() {
  const router = createRouter({
    history: createMemoryHistory(),
    routes: [
      { path: '/', component: { template: '<div />' } },
      { path: '/charging', component: { template: '<div />' } },
      { path: '/orders', component: { template: '<div />' } },
      { path: '/profile', component: { template: '<div />' } },
      { path: '/:pathMatch(.*)*', component: { template: '<div />' } }
    ]
  })
  await router.push('/charging')
  await router.isReady()
  const wrapper = mount(ChargingView, { global: { plugins: [router] } })
  return { wrapper, router }
}

let auth = null

beforeEach(() => {
  // 充电页用 setInterval 轮询订单；时间必须由用例掌控，否则断言会与轮询竞争。
  vi.useFakeTimers()
  setActivePinia(createPinia())
  auth = useAuthStore()
  setAccessToken('user-token-1')
  auth.token = 'user-token-1'
})

afterEach(() => {
  vi.useRealTimers()
  vi.unstubAllGlobals()
  clearAccessToken()
})

describe('ChargingView', () => {
  it('充电中的订单渲染进度与余额，不触发小票请求', async () => {
    const fetchMock = stubServer()
    const { wrapper } = await mountView()
    await flushPromises()

    expect(wrapper.get('[data-testid="charging-status"]').text()).toBe('充电中')
    expect(wrapper.get('[data-testid="progress-energy"]').text()).toBe('1.20 kWh')
    expect(wrapper.get('[data-testid="charging-balance"]').text()).toBe('50.00')
    expect(wrapper.find('[data-testid="charging-receipt"]').exists()).toBe(false)

    // 仍在充电：只轮询活动订单，不请求订单详情。
    const urls = fetchMock.mock.calls.map(call => call[0])
    expect(urls).not.toContain(`/api/v1/orders/${ORDER_NO}`)

    wrapper.unmount()
  })

  it('订单离开活动集合后从订单接口拉取小票并渲染', async () => {
    const fetchMock = stubServer()
    const { wrapper } = await mountView()
    await flushPromises()

    // 轮询周期到点：列表已空，说明此前那张订单到了终态。
    vi.advanceTimersByTime(3000)
    await flushPromises()

    const urls = fetchMock.mock.calls.map(call => call[0])
    expect(urls).toContain(`/api/v1/orders/${ORDER_NO}`)
    expect(wrapper.find('[data-testid="charging-error"]').exists()).toBe(false)
    expect(wrapper.get('[data-testid="charging-receipt"]').exists()).toBe(true)
    expect(wrapper.get('[data-testid="receipt-order-no"]').text()).toBe(ORDER_NO)
    expect(wrapper.get('[data-testid="receipt-amount"]').text()).toBe('1.80 元')

    // 终态后停止轮询：再过若干个周期也不该产生新的订单详情请求。
    const detailCalls = fetchMock.mock.calls.filter(call => call[0] === `/api/v1/orders/${ORDER_NO}`).length
    vi.advanceTimersByTime(9000)
    await flushPromises()
    expect(fetchMock.mock.calls.filter(call => call[0] === `/api/v1/orders/${ORDER_NO}`).length).toBe(detailCalls)

    wrapper.unmount()
  })
})
