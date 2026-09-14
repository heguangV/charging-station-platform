// 本地联调专用：代理到 18443 开发服务端并剥离 Origin（该实例未配 CORS 白名单）。
// 不入库；正式代理配置见 vite.config.ts。
import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { resolve } from 'path'

export default defineConfig({
  plugins: [vue()],
  publicDir: false,
  resolve: {
    alias: {
      '@': resolve(__dirname, 'src')
    }
  },
  server: {
    port: 3000,
    open: false,
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:18443',
        changeOrigin: true,
        configure: proxy => {
          proxy.on('proxyReq', req => {
            req.removeHeader('Origin')
            req.removeHeader('origin')
          })
        }
      }
    }
  },
  build: {
    outDir: 'dist',
    assetsDir: 'assets',
    sourcemap: false,
    rollupOptions: {
      output: {
        manualChunks: {
          'vendor-vue': ['vue', 'pinia'],
          'vendor-echarts': ['echarts/core']
        }
      }
    }
  }
})
