import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount } from '@vue/test-utils'
import { nextTick } from 'vue'
import { createPinia, setActivePinia } from 'pinia'
import { createMemoryHistory, createRouter } from 'vue-router'
import AgentView from '../src/views/AgentView.vue'
import { useStationStore } from '../src/stores/station'

/**
 * AI 助手页。服务端端点上线后页面不再是“暂未开放”的占位：它必须真正挂载聊天面板，
 * 并把服务端地址提示成可由浏览器定位或手动选择位置。
 */

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
    // 端点已上线：页面挂载真实聊天面板，而不是“暂未开放”的占位说明。
    expect(wrapper.find('[data-testid="agent-chat"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="agent-unavailable"]').exists()).toBe(false)
    expect(wrapper.get('[data-testid="agent-location-hint"]').text()).toContain('未定位')
  })

  it('定位成功后展示坐标，并说明坐标由服务端转换', async () => {
    const { wrapper } = await mountView()
    useStationStore().applyLocation(
      { latitudeE6: 39977680, longitudeE6: 116316417, coordinateType: 'wgs84', label: '北京 · 中关村' },
      'geolocation'
    )
    await nextTick()

    const hint = wrapper.get('[data-testid="agent-location-hint"]').text()
    expect(hint).toContain('北京 · 中关村')
    expect(hint).toContain('39.9777')
    expect(hint).toContain('116.3164')
    expect(hint).toContain('GCJ-02')
    expect(wrapper.find('[data-testid="agent-chat"]').exists()).toBe(true)
  })
})
