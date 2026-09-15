import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount } from '@vue/test-utils'
import { nextTick } from 'vue'
import StatCard from '../src/components/StatCard.vue'

function mountCard(props = {}) {
  return mount(StatCard, {
    props: { label: '今日营收', testId: 'overview-kpi-revenue-today', ...props }
  })
}

beforeEach(() => {
  vi.useFakeTimers()
  // 让 rAF 同步执行，使“数据更新高亮”在断言前就进入可观察状态。
  vi.stubGlobal('requestAnimationFrame', callback => {
    callback(0)
    return 1
  })
})

afterEach(() => {
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

describe('StatCard 数值格式化', () => {
  it('整数分按元展示两位小数', () => {
    const wrapper = mountCard({ format: 'amount', value: 1250 })
    expect(wrapper.get('[data-testid="overview-kpi-revenue-today-value"]').text()).toBe('12.50')
  })

  it('百分比、台数与电量分别格式化', () => {
    expect(mountCard({ format: 'percent', value: 70.83 }).get('[data-testid="overview-kpi-revenue-today-value"]').text()).toBe('70.8%')
    expect(mountCard({ format: 'count', value: 24 }).get('[data-testid="overview-kpi-revenue-today-value"]').text()).toBe('24')
    expect(mountCard({ format: 'energy', value: 1500000 }).get('[data-testid="overview-kpi-revenue-today-value"]').text()).toBe('1.50')
    expect(mountCard({ format: 'count', value: null }).get('[data-testid="overview-kpi-revenue-today-value"]').text()).toBe('—')
  })

  it('数值非法时不渲染 NaN', () => {
    const wrapper = mountCard({ format: 'amount', value: 'oops' })
    const text = wrapper.get('[data-testid="overview-kpi-revenue-today-value"]').text()
    expect(text).toBe('—')
    expect(text).not.toContain('NaN')
  })

  it('加载中显示骨架，同时保留指标卡 testid', () => {
    const wrapper = mountCard({ format: 'amount', value: 0, loading: true })
    expect(wrapper.find('[data-testid="overview-kpi-revenue-today-loading"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="overview-kpi-revenue-today"]').exists()).toBe(true)
  })

  it('提示文案渲染在 hint 中', () => {
    const wrapper = mountCard({ format: 'count', value: 3, hint: '总设备 24 台' })
    expect(wrapper.text()).toContain('总设备 24 台')
  })
})

describe('StatCard 数据更新高亮', () => {
  it('首次渲染不闪烁，数值变化后闪烁且文本始终是最终值', async () => {
    const wrapper = mountCard({ format: 'amount', value: 1250 })
    const valueNode = () => wrapper.get('[data-testid="overview-kpi-revenue-today-value"]')
    expect(valueNode().classes()).not.toContain('value-flash')
    expect(valueNode().text()).toBe('12.50')

    await wrapper.setProps({ value: 1500 })
    expect(valueNode().classes()).toContain('value-flash')
    expect(valueNode().text()).toBe('15.00')

    vi.advanceTimersByTime(1000)
    await nextTick()
    expect(valueNode().classes()).not.toContain('value-flash')
    expect(valueNode().text()).toBe('15.00')
  })

  it('数值未变化时不闪烁', async () => {
    const wrapper = mountCard({ format: 'count', value: 24 })
    await wrapper.setProps({ value: 24 })
    expect(wrapper.get('[data-testid="overview-kpi-revenue-today-value"]').classes()).not.toContain('value-flash')
  })

  it('动画不改变 data-testid 与文本内容', async () => {
    const wrapper = mountCard({ format: 'percent', value: 70.8 })
    await wrapper.setProps({ value: 71.2 })
    const node = wrapper.get('[data-testid="overview-kpi-revenue-today-value"]')
    expect(node.attributes('data-testid')).toBe('overview-kpi-revenue-today-value')
    expect(node.text()).toBe('71.2%')
  })
})
