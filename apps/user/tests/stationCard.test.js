import { describe, expect, it } from 'vitest'
import { mount } from '@vue/test-utils'
import StationCard from '../src/components/StationCard.vue'

const station = {
  id: 1,
  code: 'ZGC',
  name: 'NCS 中关村充电站',
  address: '北京市海淀区中关村大街 27 号',
  adcode: '110108',
  latitudeE6: 39977680,
  longitudeE6: 116316417,
  electricityPriceCentPerKwh: 85,
  servicePriceCentPerKwh: 50,
  totalPriceCentPerKwh: 135,
  idleCount: 3,
  operationalCount: 9,
  totalCount: 10,
  distanceMeter: 2300,
  fastChargerCount: 2,
  slowChargerCount: 4
}

describe('StationCard', () => {
  it('展示名称、地址、换算后的距离与整数分价格', () => {
    const wrapper = mount(StationCard, { props: { station } })

    expect(wrapper.get('[data-testid="station-card-name"]').text()).toBe('NCS 中关村充电站')
    expect(wrapper.get('[data-testid="station-card-address"]').text()).toBe('北京市海淀区中关村大街 27 号')
    // 2300 米 -> 2.3 km，135 分/千瓦时 -> 1.35 元/kWh（仅在显示层换算）
    expect(wrapper.get('[data-testid="station-card-distance"]').text()).toBe('2.3 km')
    expect(wrapper.get('[data-testid="station-card-price"]').text()).toBe('1.35 元/kWh')
    expect(wrapper.get('[data-testid="station-card"]').attributes('data-station-id')).toBe('1')
  })

  it('展示空闲数与总桩数，并标注快充/慢充支持', () => {
    const wrapper = mount(StationCard, { props: { station } })

    expect(wrapper.get('[data-testid="station-card-idle"]').text()).toBe('3')
    expect(wrapper.get('[data-testid="station-card-total"]').text()).toBe('10')
    expect(wrapper.get('[data-testid="station-card-fast"]').text()).toContain('快充 2 空闲')
    expect(wrapper.get('[data-testid="station-card-slow"]').text()).toContain('慢充 4 空闲')
    expect(wrapper.get('[data-testid="station-card-operational"]').text()).toContain('9')
  })

  it('缺少价格时分项相加，缺少距离时显示占位文案', () => {
    const wrapper = mount(StationCard, {
      props: { station: { id: 2, name: '测试站', electricityPriceCentPerKwh: 80, servicePriceCentPerKwh: 46 } }
    })
    expect(wrapper.get('[data-testid="station-card-price"]').text()).toBe('1.26 元/kWh')
    expect(wrapper.get('[data-testid="station-card-distance"]').text()).toBe('距离未知')
  })

  it('站点 DTO 无桩型统计时，用当前过滤条件作为快充/慢充支持证据', () => {
    const plain = { id: 3, name: '按桩型过滤出的站点', totalPriceCentPerKwh: 120, distanceMeter: 800 }
    const fastOnly = mount(StationCard, { props: { station: plain, matchedChargerType: 1 } })
    expect(fastOnly.find('[data-testid="station-card-fast"]').exists()).toBe(true)
    expect(fastOnly.find('[data-testid="station-card-slow"]').exists()).toBe(false)

    const slowOnly = mount(StationCard, { props: { station: plain, matchedChargerType: 0 } })
    expect(slowOnly.find('[data-testid="station-card-slow"]').exists()).toBe(true)
    expect(slowOnly.find('[data-testid="station-card-fast"]').exists()).toBe(false)

    const unfiltered = mount(StationCard, { props: { station: plain } })
    expect(unfiltered.find('[data-testid="station-card-fast"]').exists()).toBe(false)
    expect(unfiltered.get('[data-testid="station-card-distance"]').text()).toBe('800 米')
  })

  it('点击卡片（或详情按钮）只触发一次 select，并携带站点对象', async () => {
    const wrapper = mount(StationCard, { props: { station } })

    await wrapper.get('[data-testid="station-card"]').trigger('click')
    expect(wrapper.emitted('select')).toHaveLength(1)
    expect(wrapper.emitted('select')[0][0]).toMatchObject({ id: 1, name: 'NCS 中关村充电站' })

    await wrapper.get('[data-testid="station-card-select"]').trigger('click')
    expect(wrapper.emitted('select')).toHaveLength(2)
  })

  it('键盘 Enter 也能选中，readonly 卡片不触发事件', async () => {
    const wrapper = mount(StationCard, { props: { station } })
    await wrapper.get('[data-testid="station-card"]').trigger('keydown.enter')
    expect(wrapper.emitted('select')).toHaveLength(1)

    const readonlyWrapper = mount(StationCard, { props: { station, clickable: false } })
    await readonlyWrapper.get('[data-testid="station-card"]').trigger('click')
    expect(readonlyWrapper.emitted('select')).toBeUndefined()
  })
})
