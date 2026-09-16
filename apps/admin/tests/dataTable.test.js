import { describe, expect, it } from 'vitest'
import { mount } from '@vue/test-utils'
import DataTable from '../src/components/DataTable.vue'

const COLUMNS = [
  { key: 'code', label: '电桩编号' },
  { key: 'powerWatt', label: '功率', align: 'right', format: value => `${(value / 1000).toFixed(1)} kW` },
  { key: 'statusText', label: '状态' }
]

const ROWS = [
  { id: 1, code: 'ZGC-DC-01', powerWatt: 120000, statusText: '空闲' },
  { id: 2, code: 'ZGC-DC-02', powerWatt: 60000, statusText: '故障' }
]

function mountTable(props = {}) {
  return mount(DataTable, {
    props: {
      columns: COLUMNS,
      rows: ROWS,
      rowKey: 'id',
      testId: 'chargers',
      ...props
    }
  })
}

describe('DataTable 渲染', () => {
  it('按列定义渲染表头与数据行，行 testid 使用 data-testid 约定', () => {
    const wrapper = mountTable()
    const headers = wrapper.findAll('th').map(node => node.text())
    expect(headers).toEqual(['电桩编号', '功率', '状态'])
    expect(wrapper.find('[data-testid="chargers-table"]').exists()).toBe(true)

    const firstRow = wrapper.get('[data-testid="chargers-row-1"]')
    expect(firstRow.text()).toContain('ZGC-DC-01')
    expect(firstRow.text()).toContain('120.0 kW')
    expect(wrapper.get('[data-testid="chargers-row-2"]').text()).toContain('故障')
  })

  it('支持自定义行 testid 与右侧数字对齐', () => {
    const wrapper = mountTable({ rowTestId: row => `charger-row-${row.id}` })
    expect(wrapper.find('[data-testid="charger-row-1"]').exists()).toBe(true)
    const numericCell = wrapper.get('[data-testid="charger-row-1"] td:nth-child(2)')
    expect(numericCell.classes()).toContain('is-num')
  })

  it('选中行带 is-selected，点击行抛出完整行对象', async () => {
    const wrapper = mountTable({ selectedKey: 2 })
    expect(wrapper.get('[data-testid="chargers-row-2"]').classes()).toContain('is-selected')
    await wrapper.get('[data-testid="chargers-row-1"]').trigger('click')
    expect(wrapper.emitted('row-click')[0][0]).toMatchObject({ id: 1, code: 'ZGC-DC-01' })
  })

  it('加载中显示骨架并保留容器 testid', () => {
    const wrapper = mountTable({ loading: true, rows: [] })
    expect(wrapper.find('[data-testid="chargers-loading"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="chargers-table"]').exists()).toBe(true)
    expect(wrapper.find('tbody').exists()).toBe(false)
    expect(wrapper.find('[data-testid="chargers-empty"]').exists()).toBe(false)
  })

  it('空数据显示空状态', () => {
    const wrapper = mountTable({ rows: [] })
    expect(wrapper.get('[data-testid="chargers-empty"]').text()).toContain('暂无数据')
    expect(wrapper.find('table').exists()).toBe(false)
  })

  it('错误状态显示可重试提示并抛出 retry 事件', async () => {
    const wrapper = mountTable({ rows: [], error: '设备列表加载失败，请稍后重试' })
    const alert = wrapper.get('[data-testid="chargers-error"]')
    expect(alert.text()).toContain('设备列表加载失败')
    await alert.get('button').trigger('click')
    expect(wrapper.emitted('retry')).toHaveLength(1)
  })

  it('缺失字段一律显示占位符，绝不渲染 NaN 或 undefined', () => {
    const wrapper = mountTable({
      rows: [{ id: 3 }, { id: 4, code: null, powerWatt: undefined, statusText: '' }],
      columns: COLUMNS
    })
    expect(wrapper.text()).not.toContain('NaN')
    expect(wrapper.text()).not.toContain('undefined')
    expect(wrapper.get('[data-testid="chargers-row-3"]').text()).toContain('—')
  })

  it('格式化函数返回空值时不显示 null/NaN', () => {
    const wrapper = mountTable({
      columns: [{ key: 'balanceCent', label: '余额', format: () => null }],
      rows: [{ id: 5, balanceCent: 0 }]
    })
    expect(wrapper.get('[data-testid="chargers-row-5"]').text()).toContain('—')
  })

  it('具名插槽可覆盖单元格内容', () => {
    const wrapper = mount(DataTable, {
      props: { columns: COLUMNS, rows: ROWS, rowKey: 'id', testId: 'chargers' },
      slots: {
        'cell-statusText': '<span class="pill">{{ text }}</span>'
      }
    })
    expect(wrapper.get('[data-testid="chargers-row-1"] .pill').text()).toBe('空闲')
  })
})
