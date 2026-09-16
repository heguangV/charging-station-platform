<script setup>
import { onMounted } from 'vue'
import { useAuthStore } from '@/stores/auth'

/** 根组件只负责恢复会话；页面骨架由路由中的 AdminShell 提供。 */
const auth = useAuthStore()

onMounted(() => {
  // 页面刷新后用 sessionStorage 中的令牌恢复会话（令牌从不写入 localStorage）。
  auth.restore()
})
</script>

<template>
  <RouterView />
</template>
