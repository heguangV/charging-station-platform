import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useStationsStore } from '../src/stores/stations'
import { failResponse, installFetch, okResponse } from './helpers'

/** Go Station 契约字段。 */
const GO_STATION = {
  id: 1,
  code: 'ZGC',
  name: 'NCS 中关村站',
  address: '北京市海淀区中关村大街 27 号',
  status: 'OPEN',
  latitudeE6: 39977680,
  longitudeE6: 116316417,
  chargerCount: 10,
  idleChargerCount: 3,
  minPriceCentPerKwh: 135
}

function goPage(items, total = items.length) {
  return { items, meta: { page: 1, pageSize: 20, total } }
}

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

describe('站点列表查询（Go 契约）', () => {
  it('只提交 Go 支持的 keyword 过滤，并带上分页参数', async () => {
    harness = installFetch([okResponse(goPage([GO_STATION]))])
    await stations.setFilter({ keyword: '中关村' })

    const query = harness.queryOf(0)
    expect(harness.urlOf(0).startsWith('/api/v1/admin/stations')).toBe(true)
    expect(query.get('keyword')).toBe('中关村')
    expect(query.get('page')).toBe('1')
    expect(query.get('pageSize')).toBe('20')
    // Go 契约不支持 status/adcode 过滤：不提交，避免伪装成已生效的筛选。
    expect(query.has('status')).toBe(false)
    expect(query.has('adcode')).toBe(false)

    // 未提供过滤参数表示不过滤：重置后不应再出现该参数
    await stations.resetFilters()
    const cleared = harness.queryOf(harness.count() - 1)
    expect(cleared.has('keyword')).toBe(false)
  })

  it('加载中、失败与空数据的三种状态互斥', async () => {
    harness = installFetch([okResponse(goPage([]))])
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
    harness = installFetch([okResponse(goPage([]))])
    stations.page = 3
    await stations.setFilter({ keyword: 'ZGC' })

    expect(stations.page).toBe(1)
    expect(harness.queryOf(0).get('page')).toBe('1')
    expect(harness.queryOf(0).get('keyword')).toBe('ZGC')
  })
})

describe('站点写入（Go 契约）', () => {
  it('新增站点携带幂等键，body 收敛为 Go 字段，成功后重载列表', async () => {
    harness = installFetch([
      okResponse({ id: 9, code: 'ZGC2', name: 'NCS 中关村二站', status: 'OPEN' }),
      okResponse(goPage([GO_STATION]))
    ])
    const ok = await stations.create({
      code: 'ZGC2',
      name: 'NCS 中关村二站',
      address: '北京市海淀区示例路 1 号',
      adcode: '110108',
      latitudeE6: 39977680,
      longitudeE6: 116316417,
      businessHours: '00:00-24:00',
      initialCharger: { count: 4, chargerType: 1, powerWatt: 60000 }
    })

    expect(ok).toBe(true)
    expect(harness.indexOf('POST', '/admin/stations')).toBe(0)
    const headers = harness.headersOf(0)
    expect(headers['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    // Go 契约字段：code/name/address/latitudeE6/longitudeE6；
    // 不含 initialCharger（建设备暂无端点）、adcode、businessHours。
    expect(harness.bodyOf(0)).toEqual({
      code: 'ZGC2',
      name: 'NCS 中关村二站',
      address: '北京市海淀区示例路 1 号',
      latitudeE6: 39977680,
      longitudeE6: 116316417
    })
    // 结构性变更后必须重新拉取列表，保证总数与页面一致
    expect(harness.indexOf('GET', '/admin/stations')).toBe(1)
    expect(stations.notice).toContain('ZGC2')
  })

  it('修改站点走 PUT /admin/stations/{id}，只提交名称/地址/经纬度', async () => {
    harness = installFetch([
      okResponse({ id: 1, code: 'ZGC', name: '新名称', address: '新地址',
        status: 'OPEN', latitudeE6: 39977680, longitudeE6: 116316417 })
    ])
    stations.items = [{ ...GO_STATION }]

    await expect(
      stations.edit(stations.items[0], {
        name: '新名称',
        address: '新地址',
        latitudeE6: 39977680,
        longitudeE6: 116316417
      })
    ).resolves.toBe(true)

    expect(harness.indexOf('PUT', '/admin/stations/1')).toBe(0)
    // 编码与状态不在该端点的职责内，请求体里不能出现。
    expect(harness.bodyOf(0)).toEqual({
      name: '新名称',
      address: '新地址',
      latitudeE6: 39977680,
      longitudeE6: 116316417
    })
    // 服务端返回整条记录，但只合并它负责的四项，避免把别处刚改的字段写回旧值。
    expect(stations.items[0].name).toBe('新名称')
    expect(stations.items[0].address).toBe('新地址')
    expect(stations.items[0].code).toBe(GO_STATION.code)
    expect(stations.notice).toBe('站点信息已更新')
  })

  it('修改站点失败时保留错误提示，不改动行', async () => {
    harness = installFetch([failResponse({ status: 404, code: 4, userMessage: '站点不存在' })])
    stations.items = [{ ...GO_STATION }]
    await expect(stations.edit(stations.items[0], { name: '新名称', address: '新地址' })).resolves.toBe(false)
    expect(stations.error).toContain('站点不存在')
    expect(stations.items[0].name).toBe(GO_STATION.name)
  })

  it('站点状态变更走 PUT /status（OPEN/DISABLED），成功后就地更新该行', async () => {
    harness = installFetch([okResponse({ id: 1, code: 'ZGC', name: 'NCS 中关村站', status: 'DISABLED' })])
    stations.items = [{ ...GO_STATION, enabled: true, version: 0 }]
    await stations.setEnabled(stations.items[0], false, '设备维护停用')

    expect(harness.indexOf('PUT', '/admin/stations/1/status')).toBe(0)
    // Go 契约 body 为 status（OPEN/CLOSED/DISABLED）+ reason；启停布尔映射为 OPEN/DISABLED。
    expect(harness.bodyOf(0)).toEqual({ status: 'DISABLED', reason: '设备维护停用' })
    expect(harness.headersOf(0)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(stations.items[0].enabled).toBe(false)
    expect(harness.count()).toBe(1)
  })
})

describe('全局费率与服务费调整', () => {
  it('全局费率读取 /admin/tariffs：按配置成行并给出车队汇总', async () => {
    harness = installFetch([okResponse({
      chargerCount: 12,
      configurationCount: 2,
      minElectricityPriceCentPerKwh: 85,
      maxElectricityPriceCentPerKwh: 120,
      minServicePriceCentPerKwh: 30,
      maxServicePriceCentPerKwh: 50,
      offPeakChargerCount: 2,
      flatChargerCount: 10,
      configurations: [
        { electricityPriceCentPerKwh: 120, servicePriceCentPerKwh: 50, chargerCount: 10 },
        { electricityPriceCentPerKwh: 85, servicePriceCentPerKwh: 30,
          offPeakElectricityPriceCentPerKwh: 60, offPeakStartHour: 23, offPeakEndHour: 7,
          chargerCount: 2 }
      ]
    })])

    await expect(stations.loadTariffs()).resolves.toBe(true)

    expect(harness.indexOf('GET', '/admin/tariffs')).toBe(0)
    expect(stations.tariffs).toHaveLength(2)
    expect(stations.tariffs[1].offPeakStartHour).toBe(23)
    // 同一组价格可能重复出现，行键必须稳定且互不相同。
    expect(new Set(stations.tariffs.map(row => row.key)).size).toBe(2)
    expect(stations.tariffSummary.chargerCount).toBe(12)
    expect(stations.tariffSummary.offPeakChargerCount).toBe(2)
  })

  it('全局费率下发走 PUT /admin/tariffs，带原因并在成功后就地刷新', async () => {
    harness = installFetch([
      okResponse({ affectedChargers: 12, previousConfigurations: 3,
        electricityPriceCentPerKwh: 100, servicePriceCentPerKwh: 55 }),
      okResponse({ chargerCount: 12, configurationCount: 1, configurations: [] })
    ])

    await expect(stations.setGlobalTariff({
      electricityPriceCentPerKwh: 100, servicePriceCentPerKwh: 55, reason: '统一费率'
    })).resolves.toBe(true)

    expect(harness.indexOf('PUT', '/admin/tariffs')).toBe(0)
    expect(harness.bodyOf(0)).toEqual({
      electricityPriceCentPerKwh: 100,
      servicePriceCentPerKwh: 55,
      reason: '统一费率'
    })
    // 不带谷时字段即表示改为单一费率，请求体里不应出现半个窗口。
    expect(harness.bodyOf(0).offPeakStartHour).toBeUndefined()
    expect(stations.notice).toContain('12')
    expect(stations.notice).toContain('3')
    // 下发成功后重新读取，页面显示的是服务端的真实状态而不是本地推测。
    expect(harness.indexOf('GET', '/admin/tariffs')).toBe(1)
  })

  it('带谷时窗口时三项一起提交', async () => {
    harness = installFetch([
      okResponse({ affectedChargers: 4, previousConfigurations: 2 }),
      okResponse({ chargerCount: 4, configurationCount: 1, configurations: [] })
    ])

    await stations.setGlobalTariff({
      electricityPriceCentPerKwh: 100,
      servicePriceCentPerKwh: 55,
      offPeakElectricityPriceCentPerKwh: 60,
      offPeakStartHour: 23,
      offPeakEndHour: 7,
      reason: '峰谷分时'
    })

    expect(harness.bodyOf(0)).toEqual({
      electricityPriceCentPerKwh: 100,
      servicePriceCentPerKwh: 55,
      offPeakElectricityPriceCentPerKwh: 60,
      offPeakStartHour: 23,
      offPeakEndHour: 7,
      reason: '峰谷分时'
    })
  })

  it('按桩型的服务费比例调整仍不可用：显式失败且不发起请求', async () => {
    harness = installFetch([])
    const ok = await stations.approveAdjustment({ stationId: 1, adjustmentBp: 500 })
    expect(ok).toBe(false)
    expect(stations.error).toContain('暂未提供')
    expect(harness.count()).toBe(0)
  })
})
