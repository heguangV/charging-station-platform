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

/**
 * Go 契约订单（充电中）——**真实形态**：停止回执之前平台没有计量，
 * `amountCent` 是 0、`energyWh` 因为 omitempty 根本不出现。
 * 旧夹具在这里塞了 energyWh: 1200 / amountCent: 180，正是这个夹具让
 * "充电中金额电量恒为 0" 的显示问题一直测不出来。
 */
const ACTIVE_ORDER = {
  orderNo: ORDER_NO,
  status: 'CHARGING',
  stationId: 3,
  stationName: 'NCS 中关村充电站',
  chargerId: 28,
  chargerCode: 'A-01',
  amountCent: 0,
  createdAt: '2026-09-16T00:00:00Z',
  updatedAt: '2026-09-16T00:00:06Z'
}

const RESERVED_ORDER = {
  orderNo: ORDER_NO,
  status: 'CREATED',
  stationId: 3,
  chargerId: 28,
  amountCent: 0,
  reservedUntil: '2026-09-16T00:15:00Z',
  createdAt: '2026-09-16T00:00:00Z',
  updatedAt: '2026-09-16T00:00:00Z'
}

/**
 * 设备已经上报过计量时，订单详情会带上的字段（列表没有）：meteredEnergyWh /
 * meteredAmountCent / meteredAt。它们与结算用的 energyWh/amountCent 分开，
 * 页面必须直接显示这些真实值，而不是预估或占位符。
 */
const METERED_DETAIL = {
  meteredEnergyWh: 1200,
  meteredAmountCent: 180,
  meteredAt: '2026-09-16T00:19:30Z'
}

/** 同一张订单的终态：结算完成、等待用户确认扣款。停止回执带来了真实计量与账单。 */
const COMPLETED_ORDER = {
  ...ACTIVE_ORDER,
  status: 'COMPLETED',
  energyWh: 1200,
  amountCent: 180,
  paymentStatus: 'PENDING',
  paidCent: 0,
  updatedAt: '2026-09-16T00:30:00Z'
}

/** 确认支付后的订单：从余额扣款 1.80 元。 */
const CONFIRMED_ORDER = {
  ...COMPLETED_ORDER,
  paymentStatus: 'PAID',
  paidCent: 180
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
function stubServer(activeOrder = ACTIVE_ORDER, { keepActive = false, detail = null } = {}) {
  const listCalls = { count: 0 }
  const state = { confirmed: false }
  const fetchMock = vi.fn(async (url, init) => {
    void init
    if (url.startsWith('/api/v1/wallet')) {
      return json(state.confirmed ? { balanceCent: 4820, availableCent: 4820, debtCent: 0 } : { balanceCent: 5000, availableCent: 5000, debtCent: 0 })
    }
    if (url === `/api/v1/orders/${ORDER_NO}/confirm`) {
      state.confirmed = true
      return json(CONFIRMED_ORDER)
    }
    // 充电中的订单详情：带实时计量与估算依据（列表接口没有这些字段）
    if (url === `/api/v1/orders/${ORDER_NO}`) return json(detail ? { ...activeOrder, ...detail } : COMPLETED_ORDER)
    if (url.startsWith('/api/v1/orders')) {
      const first = listCalls.count === 0
      listCalls.count += 1
      const items = first || keepActive ? [activeOrder] : []
      return json({ items, meta: { page: 1, pageSize: 50, total: items.length } })
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
  it('预约阶段显示倒计时，并只在用户确认后发送 START', async () => {
    vi.setSystemTime(new Date('2026-09-16T00:05:00Z'))
    const fetchMock = stubServer(RESERVED_ORDER, { keepActive: true, detail: RESERVED_ORDER })
    const { wrapper } = await mountView()
    await flushPromises()

    expect(wrapper.get('[data-testid="charging-status"]').text()).toBe('已预约')
    expect(wrapper.get('[data-testid="reservation-countdown"]').text()).toContain('10:00')
    expect(wrapper.get('[data-testid="charging-cancel"]').text()).toBe('取消预约')

    await wrapper.get('[data-testid="charging-start"]').trigger('click')
    await flushPromises()
    const startCalls = fetchMock.mock.calls.filter(call => call[0] === `/api/v1/orders/${ORDER_NO}/start`)
    expect(startCalls).toHaveLength(1)
    expect(startCalls[0][1].method).toBe('POST')
    expect(startCalls[0][1].headers['Idempotency-Key']).toBeTruthy()
    wrapper.unmount()
  })

  it('充电中的订单渲染进度与余额，不触发小票请求', async () => {
    const fetchMock = stubServer()
    const { wrapper } = await mountView()
    await flushPromises()

    expect(wrapper.get('[data-testid="charging-status"]').text()).toBe('充电中')
    // 没有读数时显示占位符：0.00 kWh / 0.00 元会让用户以为计量坏了
    expect(wrapper.get('[data-testid="progress-energy"]').text()).toBe('—')
    expect(wrapper.get('[data-testid="progress-amount"]').text()).toBe('—')
    expect(wrapper.get('[data-testid="progress-metering-hint"]').exists()).toBe(true)
    expect(wrapper.get('[data-testid="charging-balance"]').text()).toBe('50.00')
    expect(wrapper.find('[data-testid="charging-receipt"]').exists()).toBe(false)

    // 仍在充电：会为"估算依据"取一次订单详情，但绝不渲染结算小票。
    const urls = fetchMock.mock.calls.map(call => call[0])
    expect(urls).toContain(`/api/v1/orders/${ORDER_NO}`)
    expect(wrapper.find('[data-testid="charging-receipt"]').exists()).toBe(false)

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

  it('待确认的小票提供确认支付；确认后扣款并更新支付状态', async () => {
    const fetchMock = stubServer()
    const { wrapper } = await mountView()
    await flushPromises()
    vi.advanceTimersByTime(3000)
    await flushPromises()

    // PENDING 是阻塞下一单的状态：提示与按钮都必须可见。
    expect(wrapper.get('[data-testid="receipt-payment"]').text()).toBe('待确认')
    expect(wrapper.get('[data-testid="receipt-confirm-hint"]').text()).toContain('阻止')

    await wrapper.get('[data-testid="receipt-confirm-payment"]').trigger('click')
    await flushPromises()

    // 请求落在契约端点上，并携带幂等键（重试复用同一键）。
    const confirmCalls = fetchMock.mock.calls.filter(call => call[0] === `/api/v1/orders/${ORDER_NO}/confirm`)
    expect(confirmCalls).toHaveLength(1)
    expect(confirmCalls[0][1].method).toBe('POST')
    expect(confirmCalls[0][1].headers['Idempotency-Key']).toBeTruthy()

    // 确认后：支付状态与余额同步刷新（50.00 - 1.80 = 48.20）。
    expect(wrapper.get('[data-testid="receipt-payment"]').text()).toBe('已支付')
    expect(wrapper.get('[data-testid="charging-balance"]').text()).toBe('48.20')
    expect(wrapper.find('[data-testid="receipt-confirm-payment"]').exists()).toBe(false)

    wrapper.unmount()
  })
})

describe('ChargingView 计量与时长', () => {
  it('设备上报计量后显示真实电量与金额（不是预估）', async () => {
    stubServer(ACTIVE_ORDER, { keepActive: true, detail: METERED_DETAIL })
    const { wrapper } = await mountView()
    await flushPromises()

    expect(wrapper.get('[data-testid="progress-energy"]').text()).toBe('1.20 kWh')
    expect(wrapper.get('[data-testid="progress-amount"]').text()).toBe('1.80 元')
    // 实时值旁标注读数时间，并且不再出现"预估"字样
    expect(wrapper.get('[data-testid="progress-metering-live"]').text()).toContain('实时计量')
    // 读数时间按本机时区显示，因此只断言形态
    expect(wrapper.get('[data-testid="progress-metering-live"]').text()).toMatch(/设备 \d{2}:\d{2}:\d{2} 读数/)
    expect(wrapper.find('[data-testid="progress-metering-hint"]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('充电时长随本地时钟推进，不冻结在订单行的 updatedAt 差值上', async () => {
    // createdAt(00:00:00Z) 与 updatedAt(00:00:06Z) 的差值是 6 秒；把系统时间也放在这一刻，
    // 再把时钟往前推：只有"实时时钟"实现才会从 6 秒变成 11 秒。
    vi.setSystemTime(new Date('2026-09-16T00:00:06Z'))
    stubServer(ACTIVE_ORDER, { keepActive: true })
    const { wrapper } = await mountView()
    await flushPromises()

    expect(wrapper.get('[data-testid="progress-duration"]').text()).toBe('6 秒')
    vi.advanceTimersByTime(5000)
    await flushPromises()
    expect(wrapper.get('[data-testid="progress-duration"]').text()).toBe('11 秒')

    // 卸载后时钟必须停掉，否则会有游离的 setInterval
    wrapper.unmount()
    vi.advanceTimersByTime(5000)
  })
})

describe('ChargingView 充电中预估', () => {
  it('拿到估算依据时按额定功率×时长显示预估，并标明依据', async () => {
    // 开始时间固定，系统时间也钉在同一刻，预估才是确定的：
    // 120kW × 600 秒 = 20 kWh；单价 150 分/kWh → 3000 分 = 30.00 元
    vi.setSystemTime(new Date('2026-09-16T00:10:00Z'))
    stubServer(ACTIVE_ORDER, {
      keepActive: true,
      detail: {
        startedAt: '2026-09-16T00:00:00Z',
        chargerPowerWatt: 120000,
        unitPriceCentPerKwh: 150
      }
    })
    const { wrapper } = await mountView()
    await flushPromises()

    expect(wrapper.get('[data-testid="progress-energy"]').text()).toBe('预估 20.00 kWh')
    expect(wrapper.get('[data-testid="progress-amount"]').text()).toBe('预估 30.00 元')
    expect(wrapper.get('[data-testid="progress-metering-hint"]').text()).toContain('暂按额定功率 120 kW × 已充时长预估')
    expect(wrapper.get('[data-testid="progress-metering-hint"]').text()).toContain('读数到达后会自动换成实时值')
    wrapper.unmount()
  })

  it('缺少估算依据时退回占位符，不编造数字', async () => {
    stubServer(ACTIVE_ORDER, { keepActive: true })
    const { wrapper } = await mountView()
    await flushPromises()

    expect(wrapper.get('[data-testid="progress-energy"]').text()).toBe('—')
    expect(wrapper.get('[data-testid="progress-amount"]').text()).toBe('—')
    expect(wrapper.get('[data-testid="progress-metering-hint"]').text()).toContain('等待设备上报计量读数')
    wrapper.unmount()
  })
})

describe('ChargingView 预估的持久性', () => {
  it('列表轮询整体覆盖订单行之后，预估仍然在，并随时长增长', async () => {
    vi.setSystemTime(new Date('2026-09-16T00:10:00Z'))
    stubServer(ACTIVE_ORDER, {
      keepActive: true,
      detail: { startedAt: '2026-09-16T00:00:00Z', chargerPowerWatt: 120000, unitPriceCentPerKwh: 150 }
    })
    const { wrapper } = await mountView()
    await flushPromises()
    expect(wrapper.get('[data-testid="progress-energy"]').text()).toBe('预估 20.00 kWh')

    // 关键：活动订单每 3 秒被列表行整体替换，而列表没有估算依据字段。
    // 依据若并进 order（而不是单独存放），这里就会退回占位符——实测就是这样丢了 12 秒。
    // advanceTimersByTime 同时推进系统时钟：+6 秒 → 606 秒 → 120kW × 606/3600 = 20.2 kWh
    vi.advanceTimersByTime(6000)
    await flushPromises()
    expect(wrapper.get('[data-testid="progress-energy"]').text()).toBe('预估 20.20 kWh')
    expect(wrapper.get('[data-testid="progress-amount"]').text()).toBe('预估 30.30 元')

    // 再走一分钟：666 秒 → 22.2 kWh；22.2 × 150 分 = 3330 分 = 33.30 元
    vi.advanceTimersByTime(60000)
    await flushPromises()
    expect(wrapper.get('[data-testid="progress-energy"]').text()).toBe('预估 22.20 kWh')
    expect(wrapper.get('[data-testid="progress-amount"]').text()).toBe('预估 33.30 元')
    wrapper.unmount()
  })
})
