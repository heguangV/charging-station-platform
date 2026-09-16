<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useRoute } from 'vue-router'
import BottomNav, { NAV_ITEMS } from './BottomNav.vue'
import NavIcon from './NavIcon.vue'
import { useAuthStore } from '@/stores/auth'
import { useStationStore } from '@/stores/station'

/**
 * 唯一页面骨架：同一套路由与视图，PC 显示左侧边栏，移动端显示底部导航。
 * 断点 900px 由 src/style.css 的 @media 规则控制视觉切换，
 * matchMedia 只用于同步 data-layout 与无障碍提示，不做任何页面分流。
 */
const DESKTOP_QUERY = '(min-width: 901px)'

const route = useRoute()
const auth = useAuthStore()
const station = useStationStore()

const isDesktop = ref(false)
let mediaQueryList = null

function applyMedia(event) {
  isDesktop.value = event.matches
}

onMounted(() => {
  if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') return
  mediaQueryList = window.matchMedia(DESKTOP_QUERY)
  isDesktop.value = mediaQueryList.matches
  if (typeof mediaQueryList.addEventListener === 'function') mediaQueryList.addEventListener('change', applyMedia)
  else if (typeof mediaQueryList.addListener === 'function') mediaQueryList.addListener(applyMedia)
})

onBeforeUnmount(() => {
  if (!mediaQueryList) return
  if (typeof mediaQueryList.removeEventListener === 'function') mediaQueryList.removeEventListener('change', applyMedia)
  else if (typeof mediaQueryList.removeListener === 'function') mediaQueryList.removeListener(applyMedia)
  mediaQueryList = null
})

const layout = computed(() => (isDesktop.value ? 'desktop' : 'mobile'))
const pageTitle = computed(() => route.meta?.title || 'NCS 充电桩车主端')
const locationLabel = computed(() => {
  const location = station.location
  if (location.status === 'loading') return '定位中…'
  if (location.status === 'success') return location.label || `已定位 ${(location.latitudeE6 / 1e6).toFixed(4)}, ${(location.longitudeE6 / 1e6).toFixed(4)}`
  if (location.status === 'failed') return '定位失败'
  if (location.status === 'unsupported') return '浏览器不支持定位'
  return '未定位'
})

function isActive(item) {
  return item.to === '/' ? route.path === '/' || route.path.startsWith('/stations') : route.path.startsWith(item.to)
}
</script>

<template>
  <div class="app-shell" data-testid="app-shell" :data-layout="layout">
    <aside class="app-sidebar" data-testid="app-sidebar" aria-label="侧边导航">
      <div class="app-sidebar__brand">
        <span class="app-sidebar__logo" aria-hidden="true"><NavIcon name="charging" :size="20" /></span>
        <div>
          <strong>NCS 充电</strong>
          <span class="app-sidebar__subtitle">车主端</span>
        </div>
      </div>

      <nav class="app-sidebar__nav">
        <RouterLink
          v-for="item in NAV_ITEMS"
          :key="item.key"
          class="app-sidebar__link"
          :class="{ 'is-active': isActive(item) }"
          :to="item.to"
          :data-testid="`sidebar-${item.key}`"
        >
          <span class="app-sidebar__icon"><NavIcon :name="item.icon" :size="18" /></span>
          <span>{{ item.label }}</span>
        </RouterLink>
      </nav>

      <div class="app-sidebar__footer">
        <p class="app-sidebar__user" data-testid="sidebar-user">{{ auth.displayName }}</p>
        <p class="app-sidebar__balance">余额 {{ (auth.balanceCent / 100).toFixed(2) }} 元</p>
      </div>
    </aside>

    <div class="app-main">
      <header class="app-topbar" data-testid="app-topbar">
        <div class="app-topbar__title">
          <h1>{{ pageTitle }}</h1>
          <p class="app-topbar__location" data-testid="topbar-location">{{ locationLabel }}</p>
        </div>
        <RouterLink class="app-topbar__action" to="/profile" data-testid="topbar-profile">
          {{ auth.isLoggedIn ? auth.displayName : '登录' }}
        </RouterLink>
      </header>

      <main class="app-content" data-testid="app-content">
        <!-- 路由切换过渡：只做位移与淡入淡出，不改变任何视图内容与状态。 -->
        <RouterView v-slot="{ Component, route: current }">
          <Transition name="page" mode="out-in">
            <component :is="Component" :key="current.path" />
          </Transition>
        </RouterView>
      </main>

      <BottomNav />
    </div>
  </div>
</template>
