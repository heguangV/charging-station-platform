import { fileURLToPath, URL } from 'node:url'
import { defineConfig, loadEnv } from 'vite'
import vue from '@vitejs/plugin-vue'

// 配置来源是仓库根目录 .env（apps/user 的上级两级），与 ncs_server 共用同一份配置。
const repositoryRoot = fileURLToPath(new URL('../../', import.meta.url))

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, repositoryRoot, ['VITE_', 'TENCENT_MAP_JS_KEY'])
  const apiTarget = env.VITE_NCS_API_TARGET || 'https://127.0.0.1:8443'

  return {
    plugins: [vue()],
    envDir: repositoryRoot,
    // 只允许 VITE_ 前缀与客户端可见的腾讯地图 JS Key 注入前端。
    // TENCENT_MAP_SERVER_KEY、AI_* 等仅服务端使用的配置绝不出现在前端产物中。
    envPrefix: ['VITE_', 'TENCENT_MAP_JS_KEY'],
    resolve: {
      alias: {
        '@': fileURLToPath(new URL('./src', import.meta.url))
      }
    },
    server: {
      port: 5173,
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
