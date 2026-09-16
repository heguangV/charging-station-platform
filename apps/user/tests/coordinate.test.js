import { describe, expect, it } from 'vitest'
import {
  e6ToGcj02,
  formatDateTime,
  formatDistance,
  formatDuration,
  formatEnergy,
  formatYuan,
  fromE6,
  gcj02ToWgs84,
  outOfChina,
  toE6,
  wgs84ToGcj02
} from '../src/services/coordinate'

describe('坐标换算 WGS-84 <-> GCJ-02', () => {
  it('北京样点偏移方向与量级符合 GCJ-02 特征（东北向偏移数百米）', () => {
    const converted = wgs84ToGcj02(39.9087, 116.3975)
    const deltaLatitude = converted.latitude - 39.9087
    const deltaLongitude = converted.longitude - 116.3975
    expect(deltaLatitude).toBeGreaterThan(0.001)
    expect(deltaLatitude).toBeLessThan(0.004)
    expect(deltaLongitude).toBeGreaterThan(0.004)
    expect(deltaLongitude).toBeLessThan(0.008)
    expect(converted.latitude).toBeCloseTo(39.9101035, 6)
    expect(converted.longitude).toBeCloseTo(116.4037436, 6)
  })

  it('公开已知样例（上海 31.1774276/121.5272106）与标准实现一致', () => {
    const converted = wgs84ToGcj02(31.1774276, 121.5272106)
    expect(converted.latitude).toBeCloseTo(31.17530398364597, 8)
    expect(converted.longitude).toBeCloseTo(121.531541859215, 8)
  })

  it('往返换算在容差内回到原点（误差小于 1e-6 度）', () => {
    const samples = [
      [39.97768, 116.316417],
      [31.230416, 121.473701],
      [22.543099, 114.057868],
      [23.135509, 113.32402]
    ]
    for (const [latitude, longitude] of samples) {
      const gcj = wgs84ToGcj02(latitude, longitude)
      const back = gcj02ToWgs84(gcj.latitude, gcj.longitude)
      expect(Math.abs(back.latitude - latitude)).toBeLessThan(1e-6)
      expect(Math.abs(back.longitude - longitude)).toBeLessThan(1e-6)
    }
  })

  it('中国大陆以外不偏移（outOfChina 直通，避免境外定位被平移）', () => {
    expect(outOfChina(35.6586, 139.7454)).toBe(true)
    expect(wgs84ToGcj02(35.6586, 139.7454)).toEqual({ latitude: 35.6586, longitude: 139.7454 })
    expect(gcj02ToWgs84(35.6586, 139.7454)).toEqual({ latitude: 35.6586, longitude: 139.7454 })
    expect(outOfChina(39.9087, 116.3975)).toBe(false)
  })

  it('非法坐标抛错，不产生静默错误结果', () => {
    expect(() => wgs84ToGcj02(Number.NaN, 116)).toThrow(RangeError)
    expect(() => gcj02ToWgs84(91, 116)).toThrow(RangeError)
  })
})

describe('整数 E6 度与显示层单位换算', () => {
  it('十进制度与 E6 整数互转', () => {
    expect(toE6(39.97768)).toBe(39977680)
    expect(toE6(116.316417)).toBe(116316417)
    expect(fromE6(39977680)).toBeCloseTo(39.97768, 10)
    expect(toE6(fromE6(116316417))).toBe(116316417)
  })

  it('E6 坐标转腾讯地图渲染坐标时按 WGS-84 处理', () => {
    const converted = e6ToGcj02(39977680, 116316417)
    expect(converted.latitude).toBeCloseTo(39.9789721, 6)
    expect(converted.longitude).toBeCloseTo(116.3225334, 6)
  })

  it('金额（分）只在显示层换算成元，距离/电量/时长同理', () => {
    expect(formatYuan(135)).toBe('1.35')
    expect(formatYuan(0)).toBe('0.00')
    expect(formatYuan(10000)).toBe('100.00')
    expect(formatDistance(2300)).toBe('2.3 km')
    expect(formatDistance(230)).toBe('230 米')
    expect(formatDistance(1000)).toBe('1.0 km')
    expect(formatEnergy(60000000)).toBe('60.00 kWh')
    expect(formatDuration(480)).toBe('8 分钟')
    expect(formatDuration(45)).toBe('45 秒')
    expect(formatDuration(4500)).toBe('1 小时 15 分')
    expect(formatDuration(0)).toBe('--')
    expect(formatDateTime(1788235200)).toMatch(/^2026\//)
  })
})
