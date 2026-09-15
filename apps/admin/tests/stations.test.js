import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useStationsStore } from '../src/stores/stations'
import { failResponse, installFetch, okResponse } from './helpers'

const STATION_ROW = { id: 1, code: 'ZGC', name: 'NCS 中关村站', adcode: '110108', enabled: true, version: 3 }

let stations = null
let harness = null

beforeEach(() => {
  setActivePinia(createPinia())
  sessionStorage.clear()
  localStorage.clear()
  stations = useStationsStore()
})

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('站点列表查询', () => {
  it('只提交非空过滤条件，并带上分页参数', async () => {
    harness = installFetch([okResponse({ items: [STATION_ROW], total: 1, page: 1, pageSize: 20 })])
    await stations.setFilter({ status: 1, adcode: '110108', keyword: '中关村' })

    const query = harness.queryOf(0)
    expect(harness.urlOf(0).startsWith('/api/v1/admin/stations')).toBe(true)
    expect(query.get('status')).toBe('1')
    expect(query.get('adcode')).toBe('110108')
    expect(query.get('keyword')).toBe('中关村')
    expect(query.get('page')).toBe('1')
    expect(query.get('pageSize')).toBe('20')

    // 未提供过滤参数表示不过滤：重置后不应再出现这三个参数
    await stations.resetFilters()
    const cleared = harness.queryOf(harness.count() - 1)
    expect(cleared.has('status')).toBe(false)
    expect(cleared.has('adcode')).toBe(false)
    expect(cleared.has('keyword')).toBe(false)
  })

  it('加载中、失败与空数据的三种状态互斥', async () => {
    harness = installFetch([okResponse({ items: [], total: 0, page: 1, pageSize: 20 })])
    const pending = stations.load()
    expect(stations.loading).toBe(true)
    expect(stations.isEmpty).toBe(false)
    await pending
    expect(stations.loading).toBe(false)
    expect(stations.isEmpty).toBe(true)

    harness.always(failResponse({ status: 503, code: 3, userMessage: '数据库暂不可用' }))
    await stations.load()
    expect(stations.error).toBe('数据库暂不可用')
    expect(stations.items).toEqual([])
    expect(stations.isEmpty).toBe(false)
  })

  it('过滤条件变化会回到第一页', async () => {
    harness = installFetch([okResponse({ items: [], total: 0, page: 1, pageSize: 20 })])
    stations.page = 3
    await stations.setFilter({ keyword: 'ZGC' })

    expect(stations.page).toBe(1)
    expect(harness.queryOf(0).get('page')).toBe('1')
    expect(harness.queryOf(0).get('keyword')).toBe('ZGC')
  })
})

describe('站点写入', () => {
  it('新增站点携带幂等键，成功后重载列表', async () => {
    harness = installFetch([
      okResponse({ id: 9, code: 'ZGC2', enabled: true, version: 1 }),
      okResponse({ items: [STATION_ROW], total: 1, page: 1, pageSize: 20 })
    ])
    const ok = await stations.create({
      code: 'ZGC2',
      name: 'NCS 中关村二站',
      address: '北京市海淀区示例路 1 号',
      adcode: '110108',
      latitudeE6: 39977680,
      longitudeE6: 116316417,
      businessHours: '00:00-24:00',
      initialCharger: { count: 4, chargerType: 1, powerWatt: 60000, connectorStandard: 'GB/T 20234.3' }
    })

    expect(ok).toBe(true)
    expect(harness.indexOf('POST', '/admin/stations')).toBe(0)
    const headers = harness.headersOf(0)
    expect(headers['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(harness.bodyOf(0).initialCharger.count).toBe(4)
    // 结构性变更后必须重新拉取列表，保证总数与页面一致
    expect(harness.indexOf('GET', '/admin/stations')).toBe(1)
    expect(stations.notice).toContain('ZGC2')
  })

  it('启用/停用成功后按返回体就地更新该行（不整表重载）', async () => {
    harness = installFetch([okResponse({ id: 1, enabled: false, version: 4 })])
    stations.items = [{ ...STATION_ROW }]
    await stations.setEnabled(stations.items[0], false, '设备维护停用')

    expect(stations.items[0].enabled).toBe(false)
    expect(stations.items[0].version).toBe(4)
    expect(harness.indexOf('POST', '/admin/stations/1/disable')).toBe(0)
    expect(harness.bodyOf(0)).toEqual({ reason: '设备维护停用', version: 3 })
    expect(harness.count()).toBe(1)
    expect(stations.notice).toBe('站点已停用')
  })

  it('版本冲突时提示并重载最新数据', async () => {
    harness = installFetch([
      failResponse({ status: 409, code: 22, userMessage: '版本落后' }),
      okResponse({ items: [{ ...STATION_ROW, version: 5, enabled: false }], total: 1, page: 1, pageSize: 20 })
    ])
    stations.items = [{ ...STATION_ROW }]
    const ok = await stations.setEnabled(stations.items[0], false, '设备维护停用')

    expect(ok).toBe(false)
    expect(stations.conflict).toContain('其他管理员')
    expect(harness.indexOf('GET', '/admin/stations')).toBe(1)
    expect(stations.items[0].version).toBe(5)
  })

  it('编辑站点提交当前 version，冲突时不覆盖本地行', async () => {
    harness = installFetch([
      okResponse({ id: 1, version: 4 }),
      failResponse({ status: 409, code: 22 })
    ])
    stations.items = [{ ...STATION_ROW }]
    await expect(stations.edit(stations.items[0], { name: '新名称' })).resolves.toBe(true)
    expect(harness.bodyOf(0)).toEqual({ name: '新名称', version: 3 })
    expect(stations.items[0].version).toBe(4)

    await expect(stations.edit(stations.items[0], { name: '另一个名称' })).resolves.toBe(false)
    expect(stations.conflict).not.toBe('')
  })
})

describe('价格版本与服务费调整', () => {
  it('价格版本列表与创建都走 admin/tariffs，创建携带幂等键', async () => {
    harness = installFetch([
      okResponse({ items: [{ adcode: '110108', electricityPriceCentPerKwh: 85, servicePriceCentPerKwh: 50, effectiveFrom: 1788235200, effectiveTo: 0 }], total: 1, page: 1, pageSize: 10 }),
      okResponse({ adcode: '110108' }),
      okResponse({ items: [], total: 0, page: 1, pageSize: 10 })
    ])
    await expect(stations.loadTariffs()).resolves.toBe(true)
    expect(stations.tariffs).toHaveLength(1)
    expect(stations.tariffs[0].electricityPriceCentPerKwh).toBe(85)

    await expect(
      stations.createTariffVersion({
        adcode: '110108',
        electricityPriceCentPerKwh: 90,
        servicePriceCentPerKwh: 55,
        effectiveFrom: 1788235200,
        effectiveTo: null,
        reason: '年度基础价格'
      })
    ).resolves.toBe(true)
    const createIndex = harness.indexOf('POST', '/admin/tariffs')
    expect(harness.headersOf(createIndex)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(stations.notice).toContain('价格版本')
  })

  it('服务费调整提交 bp 与时间窗，失败时回显 userMessage', async () => {
    harness = installFetch([failResponse({ status: 422, code: 2, userMessage: '服务费调整请求内容不符合要求' })])
    const ok = await stations.approveAdjustment({
      stationId: 1,
      chargerType: 1,
      source: 'ML_APPROVED',
      adjustmentBp: 500,
      effectiveFrom: 1788235200,
      effectiveTo: 1788256800,
      reason: '未来 6 小时负荷预测偏高'
    })
    expect(ok).toBe(false)
    expect(stations.error).toBe('服务费调整请求内容不符合要求')
    expect(harness.bodyOf(0).adjustmentBp).toBe(500)
    expect(harness.headersOf(0)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
  })
})
