import { describe, expect, it } from 'vitest'
import { mount } from '@vue/test-utils'
import ConfirmDialog from '../src/components/ConfirmDialog.vue'
import ReauthDialog from '../src/components/ReauthDialog.vue'
import FormDialog from '../src/components/FormDialog.vue'

describe('ConfirmDialog（二次确认 + 原因 + 附加字段）', () => {
  it('使用约定的 testid：confirm-dialog / confirm-reason / confirm-submit', () => {
    const wrapper = mount(ConfirmDialog, { props: { title: '停用站点', hint: '确认停用该站点？' } })
    expect(wrapper.find('[data-testid="confirm-dialog"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="confirm-reason"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="confirm-submit"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="confirm-cancel"]').exists()).toBe(true)
  })

  it('原因不足 2 个字时拒绝提交并提示，不抛事件', async () => {
    const wrapper = mount(ConfirmDialog, { props: { title: '停用站点' } })
    await wrapper.get('[data-testid="confirm-reason"]').setValue('短')
    await wrapper.get('[data-testid="confirm-submit"]').trigger('click')

    expect(wrapper.emitted('confirm')).toBeUndefined()
    expect(wrapper.get('[data-testid="confirm-error"]').text()).toContain('至少 2 个字')
  })

  it('填写原因后抛出 confirm 事件，并带上附加字段（目标设备状态）', async () => {
    const wrapper = mount(ConfirmDialog, {
      props: {
        title: '强制释放流程 FLOW-1',
        fields: [
          {
            key: 'nextChargerStatus',
            label: '释放后设备状态',
            type: 'select',
            options: [
              { value: 0, label: '空闲' },
              { value: 2, label: '故障' }
            ]
          }
        ]
      }
    })

    await wrapper.get('[data-testid="confirm-reason"]').setValue('设备计划维护')
    await wrapper.get('[data-testid="confirm-field-nextChargerStatus"]').setValue(2)
    await wrapper.get('[data-testid="confirm-submit"]').trigger('click')

    expect(wrapper.emitted('confirm')[0][0]).toEqual({
      reason: '设备计划维护',
      nextChargerStatus: 2
    })
  })

  it('取消抛出 cancel 事件且不提交', async () => {
    const wrapper = mount(ConfirmDialog, { props: { title: '停用站点' } })
    await wrapper.get('[data-testid="confirm-reason"]').setValue('设备维护停用')
    await wrapper.get('[data-testid="confirm-cancel"]').trigger('click')

    expect(wrapper.emitted('cancel')).toHaveLength(1)
    expect(wrapper.emitted('confirm')).toBeUndefined()
  })

  it('requireReason=false 时（如创建备份）无需原因即可确认', async () => {
    const wrapper = mount(ConfirmDialog, { props: { title: '创建一致性备份', requireReason: false } })
    expect(wrapper.find('[data-testid="confirm-reason"]').exists()).toBe(false)
    await wrapper.get('[data-testid="confirm-submit"]').trigger('click')
    expect(wrapper.emitted('confirm')[0][0]).toEqual({ reason: '' })
  })

  it('服务端错误通过 error 属性回显在弹窗内', () => {
    const wrapper = mount(ConfirmDialog, { props: { title: '停用站点', error: '站点存在活动流程' } })
    expect(wrapper.get('[data-testid="confirm-error"]').text()).toContain('站点存在活动流程')
  })
})

describe('ReauthDialog（敏感操作重新验证）', () => {
  it('使用约定的 testid 并在空密码时阻止提交', async () => {
    const wrapper = mount(ReauthDialog)
    expect(wrapper.find('[data-testid="reauth-dialog"]').exists()).toBe(true)
    await wrapper.get('[data-testid="reauth-submit"]').trigger('click')
    expect(wrapper.emitted('submit')).toBeUndefined()
    expect(wrapper.get('[data-testid="reauth-error"]').text()).toContain('请输入当前登录密码')
  })

  it('提交密码后抛出 submit 事件，取消抛出 cancel', async () => {
    const wrapper = mount(ReauthDialog)
    await wrapper.get('[data-testid="reauth-password"]').setValue('admin-password')
    await wrapper.get('[data-testid="reauth-submit"]').trigger('click')
    expect(wrapper.emitted('submit')[0][0]).toBe('admin-password')

    await wrapper.get('[data-testid="reauth-cancel"]').trigger('click')
    expect(wrapper.emitted('cancel')).toHaveLength(1)
  })
})

describe('FormDialog（新增/编辑表单）', () => {
  it('必填校验失败时不提交，并提示具体字段', async () => {
    const wrapper = mount(FormDialog, {
      props: {
        title: '新增站点',
        testId: 'station-create-dialog',
        fields: [
          { key: 'code', label: '站点编码', required: true },
          { key: 'count', label: '初始电桩数量', type: 'number', required: true, min: 1, max: 100 }
        ]
      }
    })
    await wrapper.get('[data-testid="station-create-dialog-submit"]').trigger('click')
    expect(wrapper.emitted('submit')).toBeUndefined()
    expect(wrapper.get('[data-testid="station-create-dialog-error"]').text()).toContain('站点编码')
  })

  it('数字字段转换为整数并按范围校验', async () => {
    const wrapper = mount(FormDialog, {
      props: {
        title: '批量创建设备',
        testId: 'charger-batch-dialog',
        fields: [
          { key: 'count', label: '创建数量', type: 'number', required: true, min: 1, max: 100 },
          { key: 'powerWatt', label: '单桩功率', type: 'number', required: true, min: 1, max: 1000000 }
        ]
      }
    })
    await wrapper.get('[data-testid="charger-batch-dialog-count"]').setValue('4')
    await wrapper.get('[data-testid="charger-batch-dialog-powerWatt"]').setValue('60000')
    await wrapper.get('[data-testid="charger-batch-dialog-submit"]').trigger('click')
    expect(wrapper.emitted('submit')[0][0]).toEqual({ count: 4, powerWatt: 60000 })

    await wrapper.get('[data-testid="charger-batch-dialog-count"]').setValue('101')
    await wrapper.get('[data-testid="charger-batch-dialog-submit"]').trigger('click')
    expect(wrapper.get('[data-testid="charger-batch-dialog-error"]').text()).toContain('不能大于 100')
  })
})
