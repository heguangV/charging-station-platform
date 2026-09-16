import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { createMemoryHistory, createRouter } from 'vue-router'
import AgentChat from '../src/components/AgentChat.vue'
import { clearAccessToken } from '../src/api/http'
import { useAgentStore } from '../src/stores/agent'
import { useStationStore } from '../src/stores/station'

// Go 契约的助手结果：stations 是契约站点字段加价格拆分与桩型构成，
// 由 api/agent.js 映射成卡片消费的展示名。
const structuredResult = {
  reply: '为你推荐 NCS 中关村充电站：快充空闲 3 个，步行 200 米有 3 家餐厅。',
  stations: [
    {
      id: 1,
      code: 'ZGC',
      name: 'NCS 中关村充电站',
      address: '北京市海淀区中关村大街 27 号',
      status: 'OPEN',
      latitudeE6: 39977680,
      longitudeE6: 116316417,
      chargerCount: 10,
      idleChargerCount: 3,
      operationalChargerCount: 9,
      fastChargerCount: 6,
      slowChargerCount: 4,
      chargerTypes: ['AC', 'DC'],
      minPriceCentPerKwh: 135,
      electricityPriceCentPerKwh: 85,
      servicePriceCentPerKwh: 50,
      distanceMeter: 2300
    }
  ],
  pois: [
    {
      id: 'POI-1',
      name: '星巴克(中关村店)',
      category: '咖啡厅',
      address: '北京市海淀区中关村大街 1 号',
      latitudeE6: 39978100,
      longitudeE6: 116316000,
      distanceMeter: 210,
      tel: ''
    }
  ],
  route: {
    destinationName: 'NCS 中关村充电站',
    distanceMeter: 2300,
    durationSecond: 480,
    provider: 'TENCENT_MAP',
    fallback: false,
    steps: [{ instruction: '向东行驶', distanceMeter: 300, durationSecond: 60 }],
    polyline: [{ latitudeE6: 39977680, longitudeE6: 116316417 }]
  },
  actions: [
    { type: 'open_station', label: '查看中关村充电站', targetId: '1' },
    { type: 'navigate', label: '导航', url: 'https://apis.map.qq.com/uri/v1/routeplan?from=1' }
  ],
  tools: ['station_search', 'poi_search'],
  llmUsed: true,
  degraded: false
}

function envelope(data) {
  return { success: true, code: 0, message: '', userMessage: '', requestId: 'req-agent', data }
}

function jsonResponse(body, status = 200) {
  return { ok: status >= 200 && status < 300, status, json: async () => body }
}

function stubFetch(handler) {
  const fetchMock = vi.fn(handler)
  vi.stubGlobal('fetch', fetchMock)
  return fetchMock
}

async function mountChat() {
  const router = createRouter({
    history: createMemoryHistory(),
    routes: [
      { path: '/', component: { template: '<div />' } },
      { path: '/stations/:stationId', component: { template: '<div />' } },
      { path: '/:pathMatch(.*)*', component: { template: '<div />' } }
    ]
  })
  await router.push('/')
  await router.isReady()
  const wrapper = mount(AgentChat, { global: { plugins: [router] }, attachTo: document.body })
  return { wrapper, router }
}

beforeEach(() => {
  setActivePinia(createPinia())
  clearAccessToken()
})

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('AgentChat 交互与状态', () => {
  it('空提示词不发送请求', async () => {
    const fetchMock = stubFetch(async () => jsonResponse(envelope(structuredResult)))
    const { wrapper } = await mountChat()

    await wrapper.get('[data-testid="agent-composer"]').trigger('submit')
    await flushPromises()

    expect(fetchMock).not.toHaveBeenCalled()
    expect(useAgentStore().messages).toHaveLength(0)
    expect(wrapper.find('[data-testid="agent-message-user"]').exists()).toBe(false)

    // 只有空白字符同样不发送。
    await wrapper.get('[data-testid="agent-input"]').setValue('   ')
    await wrapper.get('[data-testid="agent-composer"]').trigger('submit')
    expect(fetchMock).not.toHaveBeenCalled()
  })

  it('等待期间展示加载态，并在请求带上位置与坐标类型', async () => {
    let resolveFetch
    const fetchMock = stubFetch(() => new Promise(resolve => (resolveFetch = resolve)))
    const { wrapper } = await mountChat()
    useStationStore().applyLocation({ latitudeE6: 39977680, longitudeE6: 116316417, coordinateType: 'wgs84' }, 'geolocation')

    await wrapper.get('[data-testid="agent-input"]').setValue('帮我找一个附近有快充并且旁边能吃饭的充电站')
    await wrapper.get('[data-testid="agent-composer"]').trigger('submit')
    await flushPromises()

    expect(wrapper.find('[data-testid="agent-loading"]').exists()).toBe(true)
    expect(wrapper.get('[data-testid="agent-send"]').attributes('disabled')).toBeDefined()

    const [url, init] = fetchMock.mock.calls[0]
    expect(url).toBe('/api/v1/agent/chat')
    expect(init.headers['Idempotency-Key']).toBeUndefined()
    expect(JSON.parse(init.body)).toEqual({
      message: '帮我找一个附近有快充并且旁边能吃饭的充电站',
      location: { latitudeE6: 39977680, longitudeE6: 116316417 },
      coordinateType: 'wgs84'
    })

    resolveFetch(jsonResponse(envelope(structuredResult)))
    await flushPromises()
    expect(wrapper.find('[data-testid="agent-loading"]').exists()).toBe(false)
  })

  it('渲染结构化结果：站点卡片、POI 卡片、路线摘要与 actions 按钮', async () => {
    stubFetch(async () => jsonResponse(envelope(structuredResult)))
    const { wrapper, router } = await mountChat()

    await wrapper.get('[data-testid="agent-input"]').setValue('找个带餐厅的快充站')
    await wrapper.get('[data-testid="agent-composer"]').trigger('submit')
    await flushPromises()

    expect(wrapper.get('[data-testid="agent-message-user"]').text()).toContain('找个带餐厅的快充站')
    expect(wrapper.get('[data-testid="agent-message-assistant"]').text()).toContain('为你推荐 NCS 中关村充电站')
    expect(wrapper.find('[data-testid="agent-result"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="station-card"]').exists()).toBe(true)
    expect(wrapper.get('[data-testid="station-card-name"]').text()).toBe('NCS 中关村充电站')
    // 站点字段由 api 层从 Go 契约映射到卡片展示名：价格取 minPriceCentPerKwh。
    expect(wrapper.get('[data-testid="station-card"]').text()).toContain('1.35 元/kWh')
    expect(wrapper.find('[data-testid="poi-card"]').exists()).toBe(true)
    expect(wrapper.get('[data-testid="poi-card-name"]').text()).toBe('星巴克(中关村店)')
    expect(wrapper.get('[data-testid="agent-route-distance"]').text()).toBe('2.3 km')
    expect(wrapper.get('[data-testid="agent-route-duration"]').text()).toBe('8 分钟')
    expect(wrapper.get('[data-testid="agent-route-navigate"]').attributes('href')).toBe('https://apis.map.qq.com/uri/v1/routeplan?from=1')

    const actions = wrapper.findAll('[data-testid="agent-action"]')
    expect(actions.map(action => action.text())).toEqual(['查看中关村充电站', '导航'])
    expect(wrapper.get('[data-testid="agent-tools"]').text()).toContain('station_search')

    // 点击站点卡片跳转站点详情。
    await wrapper.get('[data-testid="station-card-select"]').trigger('click')
    await flushPromises()
    expect(router.currentRoute.value.path).toBe('/stations/1')

    wrapper.unmount()
  })

  it('空结果渲染空状态提示', async () => {
    stubFetch(async () => jsonResponse(envelope({ reply: '没有找到匹配结果', stations: [], pois: [], route: null, actions: [], tools: [], llmUsed: false, degraded: true })))
    const { wrapper } = await mountChat()

    await wrapper.get('[data-testid="agent-input"]').setValue('附近有按摩店吗')
    await wrapper.get('[data-testid="agent-composer"]').trigger('submit')
    await flushPromises()

    expect(wrapper.find('[data-testid="agent-empty-result"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="agent-empty-stations"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="agent-empty-pois"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="agent-empty-route"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="station-card"]').exists()).toBe(false)
    expect(wrapper.find('[data-testid="agent-degraded"]').exists()).toBe(true)

    wrapper.unmount()
  })

  it('失败时展示错误态，点击重试会重新发送同一条消息', async () => {
    let call = 0
    const fetchMock = stubFetch(async () => {
      call += 1
      if (call === 1) throw new TypeError('Failed to fetch')
      return jsonResponse(envelope(structuredResult))
    })
    const { wrapper } = await mountChat()

    await wrapper.get('[data-testid="agent-input"]').setValue('导航到最近的充电站')
    await wrapper.get('[data-testid="agent-composer"]').trigger('submit')
    await flushPromises()

    expect(wrapper.find('[data-testid="agent-error"]').exists()).toBe(true)
    expect(wrapper.get('[data-testid="agent-error-message"]').text()).toContain('网络')
    expect(wrapper.find('[data-testid="agent-result"]').exists()).toBe(false)

    await wrapper.get('[data-testid="agent-retry"]').trigger('click')
    await flushPromises()

    expect(fetchMock).toHaveBeenCalledTimes(2)
    expect(JSON.parse(fetchMock.mock.calls[1][1].body).message).toBe('导航到最近的充电站')
    expect(wrapper.find('[data-testid="agent-error"]').exists()).toBe(false)
    expect(wrapper.find('[data-testid="agent-result"]').exists()).toBe(true)

    wrapper.unmount()
  })

  it('AI 会话超时错误展示为中文超时提示', async () => {
    stubFetch(() => {
      return new Promise((resolve, reject) => {
        setTimeout(() => reject(new DOMException('The operation was aborted due to timeout', 'TimeoutError')), 0)
      })
    })
    const { wrapper } = await mountChat()

    await wrapper.get('[data-testid="agent-input"]').setValue('帮我规划路线')
    await wrapper.get('[data-testid="agent-composer"]').trigger('submit')
    await new Promise(resolve => setTimeout(resolve, 5))
    await flushPromises()

    expect(wrapper.get('[data-testid="agent-error-message"]').text()).toContain('请求超时')
    expect(useAgentStore().error).toContain('请求超时')

    wrapper.unmount()
  })

  it('示例提示词快捷入口直接发送示例内容', async () => {
    const fetchMock = stubFetch(async () => jsonResponse(envelope(structuredResult)))
    const { wrapper } = await mountChat()

    expect(wrapper.get('[data-testid="agent-example-0"]').text()).toBe('帮我找一个附近有快充并且旁边能吃饭的充电站')
    await wrapper.get('[data-testid="agent-example-0"]').trigger('click')
    await flushPromises()

    expect(JSON.parse(fetchMock.mock.calls[0][1].body)).toEqual({ message: '帮我找一个附近有快充并且旁边能吃饭的充电站' })
    expect(wrapper.find('[data-testid="agent-result"]').exists()).toBe(true)

    wrapper.unmount()
  })
})
