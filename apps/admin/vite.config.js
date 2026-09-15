import { fileURLToPath, URL } from 'node:url'
import { defineConfig, loadEnv } from 'vite'
import vue from '@vitejs/plugin-vue'

// 配置来源是仓库根目录 .env（apps/admin 的上级两级），与 ncs_server 共用同一份配置。
const repositoryRoot = fileURLToPath(new URL('../../', import.meta.url))

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, repositoryRoot, ['VITE_'])
  const apiTarget = env.VITE_NCS_API_TARGET || 'https://127.0.0.1:8443'

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
