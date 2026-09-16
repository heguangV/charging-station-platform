import { fileURLToPath, URL } from 'node:url'
import { defineConfig, loadEnv } from 'vite'
import vue from '@vitejs/plugin-vue'

// 配置来源是仓库根目录 .env（apps/admin 的上级两级），通过 VITE_NCS_API_TARGET 指向 Go API。
const repositoryRoot = fileURLToPath(new URL('../../', import.meta.url))

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, repositoryRoot, ['VITE_'])
  const apiTarget = env.VITE_NCS_API_TARGET || 'http://127.0.0.1:8080'

  return {
    plugins: [vue()],
    envDir: repositoryRoot,
    // 管理端自身不持有任何密钥：只注入 VITE_ 前缀的公开配置，
    // TENCENT_MAP_*、AI_* 等服务端密钥绝不进入管理端产物（安全基线）。
    envPrefix: ['VITE_'],
    resolve: {
      alias: {
        '@': fileURLToPath(new URL('./src', import.meta.url))
      }
    },
    server: {
      port: 5174,
      open: false,
      // 共享视觉层位于项目根之外：src/main.js 直接 import '../../shared/styles/ice-theme.css'，
      // 而该 CSS 的 url('./landscape.svg') 会被 Vite 解析为 /@fs/<repo>/apps/shared/styles/landscape.svg。
      // 默认的 serving allow list 只覆盖 apps/admin（最近的 package.json），因此该资源会被拒绝服务。
      // 显式放行整棵仓库目录：它同时覆盖应用自身与 apps/shared，范围不超过本仓库。
      fs: {
        allow: [repositoryRoot]
      },
      proxy: {
        '/api': {
          target: apiTarget,
          changeOrigin: true,
          // 本机联调使用自签名证书，仅代理层跳过校验；客户端本身不忽略证书错误。
          secure: false
        }
      }
    },
    build: {
      outDir: 'dist',
      assetsDir: 'assets',
      sourcemap: false
    }
  }
})
