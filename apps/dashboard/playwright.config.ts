import { defineConfig } from '@playwright/test'
export default defineConfig({
  testDir: './tests/ui',
  use: { baseURL: 'http://127.0.0.1:3000', headless: true, screenshot: 'only-on-failure', launchOptions: process.env.DASHBOARD_BROWSER_PATH ? { executablePath: process.env.DASHBOARD_BROWSER_PATH } : {} },
  webServer: { command: 'pnpm dev --host 127.0.0.1', url: 'http://127.0.0.1:3000', reuseExistingServer: false },
})
