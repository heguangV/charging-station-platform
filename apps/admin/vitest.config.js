import { fileURLToPath, URL } from 'node:url'
import { defineConfig } from 'vitest/config'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  plugins: [vue()],
  // 与 vite.config.js 保持一致：测试也从仓库根 .env 读取公开配置。
  envDir: fileURLToPath(new URL('../../', import.meta.url)),
  envPrefix: ['VITE_'],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url))
    }
  },
  test: {
    environment: 'jsdom',
    include: ['tests/**/*.test.js'],
    setupFiles: ['tests/setup.js'],
    restoreMocks: true,
    clearMocks: true
  }
})
