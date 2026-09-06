import { defineConfig } from '@playwright/test'
const port = Number(process.env.DASHBOARD_TEST_PORT || 3000)
export default defineConfig({
  testDir: './tests/ui',
  use: { baseURL: `http://127.0.0.1:${port}`, headless: true, screenshot: 'only-on-failure', launchOptions: process.env.DASHBOARD_BROWSER_PATH ? { executablePath: process.env.DASHBOARD_BROWSER_PATH } : {} },
  webServer: { command: `pnpm dev --host 127.0.0.1 --port ${port} --strictPort`, url: `http://127.0.0.1:${port}`, reuseExistingServer: false },
})
