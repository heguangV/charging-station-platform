/**
 * 订单页申诉入口的回归测试。
 *
 * 背景：契约规定"一单一申诉"——同一订单不同内容的重复提交返回 409。
 * 视图此前把 409 直接当成"申诉已提交"，用户会以为这次填的内容也被受理了；
 * 实际上只有第一次的内容存下来了。
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { createMemoryHistory, createRouter } from 'vue-router'
import OrdersView from '../src/views/OrdersView.vue'
import { setAccessToken, clearAccessToken } from '../src/api/http'
import { useAuthStore } from '../src/stores/auth'

const ORDER_NO = 'ORD20260916000000abcd'

/** Go 契约订单（已完成、已支付）：订单页的申诉入口只对 COMPLETED 展示。 */
const COMPLETED_ORDER = {
  orderNo: ORDER_NO,
  userId: 32,
  stationId: 3,
  chargerId: 28,
  status: 'COMPLETED',
  amountCent: 180,
  paidCent: 180,
  paymentStatus: 'PAID',
  energyWh: 1200,
  createdAt: '2026-09-16T00:00:00Z',
  updatedAt: '2026-09-16T00:30:00Z'
}

function envelope(data, { status = 200, code = 0, message = 'ok' } = {}) {
  const success = code === 0 && status < 300
  return {
    ok: success,
    status,
    json: async () => ({ success, code, message, userMessage: '', requestId: 'req-1', data: success ? data : null })
  }
}

/**
 * 服务端替身：订单列表与详情正常；GET 申诉按 existingAppeal 作答（null = 404 无申诉），
 * POST 申诉按传入响应作答。
 */
function stubServer(appealResponse, { existingAppeal = null, review = null, appealGetMissesFirst = false } = {}) {
  let appealGets = 0
  return vi.fn(async (url, init = {}) => {
    if (String(url).includes('/appeal')) {
      const method = (init.method || 'GET').toUpperCase()
      if (method === 'GET') {
        appealGets += 1
        // 打开订单时还没有申诉（竞态：提交那一刻才被别处抢先提交）
        if (appealGetMissesFirst && appealGets === 1) {
          return envelope(null, { status: 404, code: 4, message: 'not found' })
        }
        return existingAppeal ? envelope(existingAppeal) : envelope(null, { status: 404, code: 4, message: 'not found' })
      }
      return appealResponse
    }
    if (String(url).includes('/review')) {
      return review ? envelope(review) : envelope(null, { status: 404, code: 4, message: 'not found' })
    }
    if (/\/api\/v1\/orders\/[^/]+$/.test(String(url))) return envelope(COMPLETED_ORDER)
    if (String(url).startsWith('/api/v1/orders')) {
      return envelope({ items: [COMPLETED_ORDER], meta: { page: 1, pageSize: 20, total: 1 } })
    }
    if (String(url).startsWith('/api/v1/stations')) return envelope({ items: [], meta: { page: 1, pageSize: 50, total: 0 } })
    if (String(url).startsWith('/api/v1/me')) return envelope({ id: 32, displayName: '车主' })
    if (String(url).startsWith('/api/v1/wallet')) return envelope({ balanceCent: 5000, availableCent: 5000 })
    void init
    return envelope({})
  })
}

async function mountView() {
  const renderErrors = []
  const router = createRouter({
    history: createMemoryHistory(),
    routes: [
      { path: '/', component: { template: '<div />' } },
      { path: '/orders', component: { template: '<div />' } },
      { path: '/charging', component: { template: '<div />' } },
      { path: '/profile', component: { template: '<div />' } },
      { path: '/:pathMatch(.*)*', component: { template: '<div />' } }
    ]
  })
  await router.push('/orders')
  await router.isReady()
  const wrapper = mount(OrdersView, {
    global: { plugins: [router], config: { errorHandler: error => renderErrors.push(error) } }
  })
  await flushPromises()
  // 展开第一张订单，渲染小票与申诉入口
  await wrapper.get(`[data-testid="order-${ORDER_NO}"]`).trigger('click')
  await flushPromises()
  return { wrapper, renderErrors }
}

beforeEach(() => {
  setActivePinia(createPinia())
  const auth = useAuthStore()
  setAccessToken('user-token-1')
  auth.token = 'user-token-1'
})

afterEach(() => {
  vi.unstubAllGlobals()
  clearAccessToken()
})

describe('订单申诉入口', () => {
  it('提交成功时显示已提交，并提示审核通过后退款', async () => {
    vi.stubGlobal('fetch', stubServer(envelope({ id: 1, orderNo: ORDER_NO, status: 'PENDING', reason: '计量争议' }, { status: 201 })))
    const { wrapper } = await mountView()

    await wrapper.get('[data-testid="appeal-reason"]').setValue('计量争议')
    await wrapper.get('[data-testid="appeal-submit"]').trigger('submit')
    await flushPromises()

    // 提交成功后直接进入"状态"视图：比一句"已提交"更能说明后续怎么走
    expect(wrapper.get('[data-testid="order-appeal-status"]').text()).toBe('审核中')
    expect(wrapper.find('[data-testid="appeal-reason"]').exists()).toBe(false)
    expect(wrapper.find('[data-testid="order-appeal-conflict"]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('409（该订单已有申诉）不能显示成"已提交"', async () => {
    vi.stubGlobal(
      'fetch',
      stubServer(envelope(null, { status: 409, code: 5, message: 'already exists with different content or state' }), {
        // 竞态：打开订单时还没有申诉，提交那一刻别处已抢先提交了一条不同内容的
        appealGetMissesFirst: true,
        existingAppeal: { id: 9, orderNo: ORDER_NO, reason: '早先那条申诉', status: 'PENDING', orderAmountCent: 180, orderPaidCent: 180 }
      })
    )
    const { wrapper } = await mountView()

    await wrapper.get('[data-testid="appeal-reason"]').setValue('换一条不同的理由')
    await wrapper.get('[data-testid="appeal-submit"]').trigger('submit')
    await flushPromises()

    // 409 后重新查询拿到真实状态（服务端返回既有申诉），并明确告知本次内容没被保存
    expect(wrapper.get('[data-testid="order-appeal-conflict"]').text()).toContain('本次填写的内容没有提交')
    expect(wrapper.find('[data-testid="appeal-reason"]').exists()).toBe(false)
    wrapper.unmount()
  })
})

describe('订单详情的渲染守卫', () => {
  it('点开订单时 receipt 尚未返回也不能抛渲染错误', async () => {
    vi.stubGlobal('fetch', stubServer(envelope({ id: 1, status: 'PENDING' }, { status: 201 })))
    const { wrapper, renderErrors } = await mountView()

    // 详情请求在途时 receipt 为 null：以前 `receipt.status === 'COMPLETED'` 会抛
    // "Cannot read properties of null (reading 'status')"，评测/申诉两个盒子都渲染不出来。
    expect(renderErrors).toEqual([])
    expect(wrapper.get('[data-testid="order-appeal"]').exists()).toBe(true)
    expect(wrapper.get('[data-testid="order-review"]').exists()).toBe(true)
    wrapper.unmount()
  })
})

describe('本人申诉状态', () => {
  it('审核中：显示状态与申诉内容，不再摆出申诉表单，也不能评价', async () => {
    vi.stubGlobal(
      'fetch',
      stubServer(envelope({ id: 9, status: 'PENDING' }, { status: 201 }), {
        existingAppeal: { id: 9, orderNo: ORDER_NO, reason: '计量争议', status: 'PENDING', orderAmountCent: 180, orderPaidCent: 180 }
      })
    )
    const { wrapper } = await mountView()

    expect(wrapper.find('[data-testid="appeal-reason"]').exists()).toBe(false)
    const state = wrapper.get('[data-testid="order-appeal-state"]')
    expect(state.text()).toContain('审核中')
    expect(state.text()).toContain('计量争议')
    // 契约：有申诉的订单不能再评价
    expect(wrapper.get('[data-testid="order-review-blocked"]').text()).toContain('生效中的申诉')
    expect(wrapper.find('[data-testid="review-submit"]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('已驳回：显示处理意见，并恢复评价入口', async () => {
    vi.stubGlobal(
      'fetch',
      stubServer(envelope({ id: 9, status: 'PENDING' }, { status: 201 }), {
        existingAppeal: {
          id: 9,
          orderNo: ORDER_NO,
          reason: '计量争议',
          status: 'REJECTED',
          decisionReason: '计量与设备记录一致，申诉不成立',
          orderAmountCent: 180,
          orderPaidCent: 180
        }
      })
    )
    const { wrapper } = await mountView()

    expect(wrapper.get('[data-testid="order-appeal-status"]').text()).toBe('已驳回')
    expect(wrapper.get('[data-testid="order-appeal-decision-reason"]').text()).toContain('计量与设备记录一致')
    // 驳回的申诉不再阻止评价
    expect(wrapper.find('[data-testid="order-review-blocked"]').exists()).toBe(false)
    expect(wrapper.get('[data-testid="review-submit"]').exists()).toBe(true)
    wrapper.unmount()
  })

  it('已通过：明确告知退款已退回', async () => {
    vi.stubGlobal(
      'fetch',
      stubServer(envelope({ id: 9, status: 'PENDING' }, { status: 201 }), {
        existingAppeal: { id: 9, orderNo: ORDER_NO, reason: '计量争议', status: 'APPROVED', orderAmountCent: 180, orderPaidCent: 180 }
      })
    )
    const { wrapper } = await mountView()

    expect(wrapper.get('[data-testid="order-appeal-status"]').text()).toContain('已通过')
    expect(wrapper.get('[data-testid="order-appeal-status"]').text()).toContain('退回钱包')
    wrapper.unmount()
  })
})

describe('已通过后的申诉可见性', () => {
  it('审核通过把订单变已取消后，用户仍能看到"已通过、已退款"', async () => {
    // 通过申诉会让订单从 COMPLETED 变 CANCELLED：申诉盒不能因此消失，
    // 否则用户只看到一个莫名其妙的"已取消"。
    const cancelled = { ...COMPLETED_ORDER, status: 'CANCELLED' }
    const fetchMock = stubServer(
      envelope({ id: 9, status: 'PENDING' }, { status: 201 }),
      { existingAppeal: { id: 9, orderNo: ORDER_NO, reason: '计量争议', status: 'APPROVED', orderAmountCent: 180, orderPaidCent: 180 } }
    )
    vi.stubGlobal('fetch', vi.fn(async (url, init = {}) => {
      if (/\/api\/v1\/orders\/[^/]+$/.test(String(url))) return envelope(cancelled)
      return fetchMock(url, init)
    }))

    const { wrapper } = await mountView()
    expect(wrapper.get('[data-testid="order-appeal-status"]').text()).toContain('已通过')
    expect(wrapper.get('[data-testid="order-appeal-status"]').text()).toContain('退回钱包')
    wrapper.unmount()
  })
})
