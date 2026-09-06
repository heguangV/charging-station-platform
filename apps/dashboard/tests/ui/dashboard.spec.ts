import { test, expect, type Page } from '@playwright/test'
import { summary, envelope } from '../fixture'
async function login(page: Page) {
  await page.route('**/api/v1/dashboard/auth/login', route => route.fulfill({ json: envelope({ accessToken: 'ui-test-token', expiresAt: Math.floor(Date.now() / 1000) + 28800, sessionId: 1 }) }))
  await page.goto('/')
  await page.getByLabel('账号', { exact: true }).fill('viewer')
  await page.getByLabel('密码', { exact: true }).fill('test-password')
  await page.getByRole('button', { name: '登录', exact: true }).click()
}
for (const [width, height] of [[1920, 1080], [1366, 768], [1280, 1024], [2560, 1080]]) {
  test(`all panels fit at ${width}x${height}`, async ({ page }) => {
    await page.setViewportSize({ width, height })
    await page.route('**/api/v1/dashboard/summary', route => route.fulfill({ json: envelope(summary()) }))
    await login(page)
    await expect(page.locator('.dashboard-main')).toBeVisible()
    await expect(page.getByText('实时在线', { exact: true })).toBeVisible()
    const boxes = await page.locator('.column, .tech-card, .metric-card').evaluateAll(elements => elements.map(el => {
      const r = el.getBoundingClientRect()
      return { x: r.x, y: r.y, right: r.right, bottom: r.bottom, overflow: el.scrollWidth - el.clientWidth }
    }))
    for (const b of boxes) {
      expect(b.x).toBeGreaterThanOrEqual(-1)
      expect(b.y).toBeGreaterThanOrEqual(-1)
      expect(b.right).toBeLessThanOrEqual(width + 1)
      expect(b.bottom).toBeLessThanOrEqual(height + 1)
      expect(b.overflow).toBeLessThanOrEqual(1)
    }
    await page.screenshot({ path: `test-results/dashboard-${width}x${height}.png` })
  })
}
test('empty, failed, recovered and unauthorized states are truthful', async ({ page }) => {
  let status = 503
  let requests = 0
  await page.route('**/api/v1/dashboard/summary', route => { requests++; return route.fulfill({ status, json: status === 200 ? envelope(summary()) : {} }) })
  const urls: string[] = []
  page.on('request', request => urls.push(request.url()))
  await login(page)
  await expect(page.getByText('数据加载失败，尚无可用快照，请重试。')).toBeVisible()
  await expect(page.locator('.metric-value').first()).toContainText('--')
  status = 200
  await page.getByRole('button', { name: '立即重试' }).click()
  await expect(page.locator('.metric-value').first()).toContainText('0')
  await expect(page.getByText('暂无数据', { exact: true }).first()).toBeVisible()
  await expect(page.getByText('1.50 元/度', { exact: true })).toHaveCount(0)
  status = 403
  await page.getByTitle('手动刷新').click()
  await expect(page.getByRole('button', { name: '登录', exact: true })).toBeVisible()
  await expect(page.locator('.dashboard-main')).toHaveCount(0)
  const count = requests
  await page.clock.install()
  await page.clock.fastForward(60_000)
  expect(requests).toBe(count)
  expect(urls.some(url => url.includes('/data/dashboard.json') || url.endsWith('/snapshot'))).toBe(false)
})
test('station names cannot inject tooltip HTML and mWh converts to kWh', async ({ page }) => {
  await page.setViewportSize({ width: 1920, height: 1080 })
  const dto = summary()
  dto.stationRanking = [{ stationId: 1, stationName: '<img src=x onerror="window.__injected=true">', energyMwh: 2_500_000, revenueCent: 500, orderCount: 1 }]
  await page.route('**/api/v1/dashboard/summary', route => route.fulfill({ json: envelope(dto) }))
  await login(page)
  const canvas = page.locator('.station-rank-card canvas')
  await expect(canvas).toBeVisible()
  await canvas.hover({ position: { x: 310, y: 210 } })
  await expect(page.getByText('总充电量:', { exact: false })).toContainText('2.5 kWh')
  await expect(page.locator('.station-rank-card img')).toHaveCount(0)
  expect(await page.evaluate(() => (window as any).__injected)).toBeUndefined()
})

test('nonempty backend DTOs render all supported charts', async ({ page }) => {
  await page.setViewportSize({ width: 1920, height: 1080 })
  const dto = summary()
  dto.totalChargeCount = 4268
  dto.totalRevenueCent = 9800000
  dto.registeredUserCount = 1280
  dto.stationCount = 5
  dto.chargerStatus = { idle: 32, inUse: 18, fault: 3, restarting: 1, disabled: 0 }
  dto.revenue30d = Array.from({ length: 30 }, (_, i) => ({ bucketAt: dto.generatedAt - (29 - i) * 86400, revenueCent: 30000 + i * 1000, energyMwh: 20_000_000 + i * 1_000_000, orderCount: 20 + i }))
  dto.stationRanking = [{ stationId: 1, stationName: '测试电站', energyMwh: 250_000_000, revenueCent: 40000, orderCount: 40 }]
  dto.hourlyHeatmap = Array.from({ length: 24 }, (_, hour) => ({ weekday: 1, hour, energyMwh: (hour + 1) * 1_000_000, orderCount: hour + 1 }))
  dto.prediction24h = Array.from({ length: 24 }, (_, i) => ({ stationId: 1, horizonHour: 24, modelVersionNo: 'TEST', generatedAt: dto.generatedAt, targetAt: dto.generatedAt + (i + 1) * 3600, predictedEnergyMwh: (i + 1) * 1_000_000, predictedFreeCount: 25 - i, isPeak: i > 20, stale: false }))
  const errors: string[] = []
  page.on('pageerror', error => errors.push(error.message))
  await page.route('**/api/v1/dashboard/summary', route => route.fulfill({ json: envelope(dto) }))
  await login(page)
  await expect(page.locator('.metric-value').first()).toContainText('4,268')
  await expect(page.locator('canvas')).toHaveCount(5)
  await expect(page.getByLabel('预测电站')).toHaveValue('1')
  // Wait for ECharts' initial series animation before recording visual evidence.
  await page.waitForTimeout(1200)
  await page.screenshot({ path: 'test-results/dashboard-populated.png' })
  expect(errors).toEqual([])
})
