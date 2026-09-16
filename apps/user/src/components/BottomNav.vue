<script>
/**
 * 5 个主导航目的地：移动端底部导航与 PC 侧边栏共用同一份定义，
 * 不存在独立的移动端路由或第二套页面。
 */
export const NAV_ITEMS = [
  { key: 'home', to: '/', label: '附近', icon: 'home', title: '附近充电站' },
  { key: 'charging', to: '/charging', label: '充电', icon: 'charging', title: '充电流程' },
  { key: 'agent', to: '/agent', label: 'AI 助手', icon: 'agent', title: 'AI 助手' },
  { key: 'orders', to: '/orders', label: '订单', icon: 'orders', title: '我的订单' },
  { key: 'profile', to: '/profile', label: '我的', icon: 'profile', title: '我的' }
]
</script>

<script setup>
import { useRoute } from 'vue-router'
import NavIcon from './NavIcon.vue'

const route = useRoute()

function isActive(item) {
  return item.to === '/' ? route.path === '/' || route.path.startsWith('/stations') : route.path.startsWith(item.to)
}
</script>

<template>
  <nav class="bottom-nav" data-testid="bottom-nav" aria-label="主导航">
    <RouterLink
      v-for="item in NAV_ITEMS"
      :key="item.key"
      class="bottom-nav__item"
      :class="{ 'is-active': isActive(item) }"
      :to="item.to"
      :data-testid="`bottom-nav-${item.key}`"
      :aria-current="isActive(item) ? 'page' : undefined"
    >
      <span class="bottom-nav__icon"><NavIcon :name="item.icon" :size="21" /></span>
      <span class="bottom-nav__label">{{ item.label }}</span>
    </RouterLink>
  </nav>
</template>
