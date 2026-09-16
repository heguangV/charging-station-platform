/**
 * 站点页「预约充电」的回归测试。
 *
 * UC-U-07 要求预约与启动是两个阶段：站点页只绑定并保留设备，用户到达后在
 * 预约页明确点击开始充电。这里锁定预约按钮绝不偷偷发送 START。
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { createMemoryHistory, createRouter } from 'vue-router'
import StationView from '../src/views/StationView.vue'
import { useAuthStore } from '../src/stores/auth'
import { clearAccessToken, setAccessToken } from '../src/api/http'

const STATION = {
  id: 1,
  code: 'ST-1',
  name: '测试站',
  address: '测试地址',
  status: 'OPEN',
  latitudeE6: 30545200,
  longitudeE6: 104070800,
  chargerCount: 1,
  idleChargerCount: 1,
  minPriceCentPerKwh: 100
}

const CHARGER = { id: 11, stationId: 1, code: 'A01', type: 'DC', powerWatt: 120000, status: 'IDLE' }

function envelope(data, { status = 200, code = 0 } = {}) {
  const success = code === 0 && status < 300
  return {
    ok: success,
    status,
    json: async () => ({ success, code, message: success ? 'ok' : 'fail', userMessage: '', requestId: 'req-1', data: success ? data : null })
  }
}

function stubServer({ createFails = false } = {}) {
  const calls = []
  const fetchMock = vi.fn(async (url, init = {}) => {
    const path = String(url)
    const method = init.method || 'GET'
    calls.push(`${method} ${path}`)
    if (method === 'POST' && path.endsWith('/api/v1/orders')) {
      return createFails
        ? envelope(null, { status: 409, code: 8 })
        : envelope({ orderNo: 'ORD-1', status: 'CREATED', amountCent: 0, reservedUntil: '2026-09-16T00:15:00Z' })
    }
    if (path.includes('/chargers')) return envelope({ items: [CHARGER], meta: { page: 1, pageSize: 50, total: 1 } })
    if (path.includes('/stations/1')) return envelope(STATION)
    if (path.includes('/stations')) return envelope({ items: [STATION], meta: { page: 1, pageSize: 50, total: 1 } })
    if (path.includes('/reviews') || path.includes('/comments')) return envelope({ items: [], meta: { page: 1, pageSize: 20, total: 0 } })
    return envelope({})
  })
  vi.stubGlobal('fetch', fetchMock)
  return calls
}

async function mountView() {
  const router = createRouter({
    history: createMemoryHistory(),
    routes: [
      { path: '/', component: { template: '<div />' } },
      { path: '/stations/:stationId', name: 'station', component: { template: '<div />' } },
      { path: '/charging', name: 'charging', component: { template: '<div />' } },
      { path: '/profile', component: { template: '<div />' } },
      { path: '/:pathMatch(.*)*', component: { template: '<div />' } }
    ]
  })
  await router.push('/stations/1')
  await router.isReady()
  const wrapper = mount(StationView, { global: { plugins: [router] } })
  await flushPromises()
  return { wrapper, router }
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

describe('站点页预约充电', () => {
  it('只创建预约，不发送 START，然后进入预约页', async () => {
    const calls = stubServer()
    const { wrapper, router } = await mountView()

    expect(wrapper.get('[data-testid="station-reserve-charging"]').text()).toBe('预约充电')
    await wrapper.get('[data-testid="station-reserve-charging"]').trigger('click')
    await flushPromises()

    const createIndex = calls.findIndex(call => call === 'POST /api/v1/orders')
    expect(createIndex).toBeGreaterThanOrEqual(0)
    expect(calls.some(call => /\/start$/.test(call))).toBe(false)
    expect(router.currentRoute.value.path).toBe('/charging')
    wrapper.unmount()
  })

  it('预约失败时停留在站点页并显示错误', async () => {
    const calls = stubServer({ createFails: true })
    const { wrapper, router } = await mountView()

    await wrapper.get('[data-testid="station-reserve-charging"]').trigger('click')
    await flushPromises()

    expect(calls).toContain('POST /api/v1/orders')
    expect(router.currentRoute.value.path).toBe('/stations/1')
    expect(wrapper.get('[data-testid="station-action-error"]').text()).not.toBe('')
    wrapper.unmount()
  })
})
