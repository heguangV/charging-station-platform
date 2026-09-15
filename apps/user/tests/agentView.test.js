import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { createMemoryHistory, createRouter } from 'vue-router'
import AgentView from '../src/views/AgentView.vue'
import { useStationStore } from '../src/stores/station'

function envelope(data) {
  return { success: true, code: 0, message: '', userMessage: '', requestId: 'req-view', data }
}

function stubFetch(handler) {
  const fetchMock = vi.fn(handler)
  vi.stubGlobal('fetch', fetchMock)
  return fetchMock
}

async function mountView() {
  const router = createRouter({
    history: createMemoryHistory(),
    routes: [
      { path: '/', component: { template: '<div />' } },
      { path: '/agent', component: { template: '<div />' } },
      { path: '/stations/:stationId', component: { template: '<div />' } },
      { path: '/:pathMatch(.*)*', component: { template: '<div />' } }
    ]
  })
  await router.push('/agent')
  await router.isReady()
  const wrapper = mount(AgentView, { global: { plugins: [router] } })
  return { wrapper, router }
}

beforeEach(() => {
  setActivePinia(createPinia())
})

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('AgentView', () => {
  it('渲染聊天面板；未定位时提示手动获取位置', async () => {
    const { wrapper } = await mountView()

    expect(wrapper.find('[data-testid="agent-view"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="agent-chat"]').exists()).toBe(true)
    expect(wrapper.get('[data-testid="agent-location-hint"]').text()).toContain('未定位')
  })

  it('定位成功后展示 WGS-84 坐标，并把坐标随消息发给服务端', async () => {
    const fetchMock = stubFetch(async () =>
      fetchMockResult({
        reply: '已为你规划到 NCS 中关村充电站的路线。',
        stations: [],
        pois: [],
        route: {
          destinationName: 'NCS 中关村充电站',
          distanceMeter: 2300,
          durationSecond: 480,
          provider: 'TENCENT_MAP',
          fallback: false,
          steps: [],
          polyline: []
        },
        actions: [{ type: 'navigate', label: '导航', url: 'https://apis.map.qq.com/uri/v1/routeplan?from=1' }],
        tools: ['route_plan'],
        llmUsed: true,
        degraded: false
      })
    )

    const { wrapper } = await mountView()
    useStationStore().applyLocation({ latitudeE6: 39977680, longitudeE6: 116316417, coordinateType: 'wgs84', label: '北京 · 中关村' }, 'geolocation')
    await flushPromises()

    expect(wrapper.get('[data-testid="agent-location-hint"]').text()).toContain('北京 · 中关村')

    await wrapper.get('[data-testid="agent-input"]').setValue('导航到最近的充电站')
    await wrapper.get('[data-testid="agent-composer"]').trigger('submit')
    await flushPromises()

    // 只访问服务端 AI 助手接口，前端不直接调用 LLM 或腾讯地图 WebService。
    expect(fetchMock).toHaveBeenCalledTimes(1)
    const [url, init] = fetchMock.mock.calls[0]
    expect(url).toBe('/api/v1/user/agent/chat')
    expect(JSON.parse(init.body)).toEqual({
      message: '导航到最近的充电站',
      location: { latitudeE6: 39977680, longitudeE6: 116316417 },
      coordinateType: 'wgs84'
    })

    expect(wrapper.get('[data-testid="agent-route-distance"]').text()).toBe('2.3 km')
    expect(wrapper.get('[data-testid="agent-route-navigate"]').attributes('target')).toBe('_blank')
    expect(wrapper.get('[data-testid="agent-route-navigate"]').attributes('rel')).toContain('noopener')

    wrapper.unmount()
  })

  it('未定位时不发送 location 字段（服务端按默认位置或确定性结果处理）', async () => {
    const fetchMock = stubFetch(async () =>
      fetchMockResult({ reply: '附近有 2 个充电站。', stations: [], pois: [], route: null, actions: [], tools: [], llmUsed: false, degraded: true })
    )
    const { wrapper } = await mountView()

    await wrapper.get('[data-testid="agent-input"]').setValue('附近有哪些充电站')
    await wrapper.get('[data-testid="agent-composer"]').trigger('submit')
    await flushPromises()

    expect(JSON.parse(fetchMock.mock.calls[0][1].body)).toEqual({ message: '附近有哪些充电站' })
    expect(wrapper.find('[data-testid="agent-degraded"]').exists()).toBe(true)

    wrapper.unmount()
  })
})

function fetchMockResult(data) {
  return { ok: true, status: 200, json: async () => envelope(data) }
}
