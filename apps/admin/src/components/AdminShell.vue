<script setup>
/**
 * 管理端唯一页面骨架。
 *
 * 桌面优先：>= 1024px 显示常驻左栏（可折叠），< 1024px 侧栏变成由顶栏拉出的抽屉。
 * 断点只由 CSS 的 @media 与这里的 matchMedia 同步（用于 aria 与抽屉行为），
 * 不存在第二套页面、不做 UA 嗅探。
 * 路由切换用 <Transition name="page" mode="out-in">，与用户端同一套动效类。
 */
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import SideRail from './SideRail.vue'
import TopBar from './TopBar.vue'
import ReauthDialog from './ReauthDialog.vue'
import { useAuthStore } from '@/stores/auth'

/** 与 src/style.css 的断点严格一致。 */
const DESKTOP_QUERY = '(min-width: 1024px)'

const route = useRoute()
const router = useRouter()
const auth = useAuthStore()

const isDesktop = ref(false)
const drawerOpen = ref(false)
const railCollapsed = ref(false)
let mediaQueryList = null

function applyMedia(event) {
  isDesktop.value = event.matches
  if (event.matches) drawerOpen.value = false
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

// 路由变化后自动收起抽屉，避免窄屏上遮住新页面。
watch(
  () => route.fullPath,
  () => {
    drawerOpen.value = false
  }
)

// 会话失效（401/403）后立即回到登录页，而不是停留在空数据页面。
watch(
  () => auth.isLoggedIn,
  logged => {
    if (!logged && route.meta?.public !== true) router.replace({ path: '/login' })
  }
)

const layout = computed(() => (isDesktop.value ? 'desktop' : 'narrow'))
const pageTitle = computed(() => route.meta?.title || '运营总览')
const crumb = computed(() => route.meta?.crumb || 'NCS 充电运营中心')

async function handleLogout() {
  await auth.logout()
  router.replace({ path: '/login' })
}

function submitReauth(password) {
  auth.submitReauth(password)
}
</script>

<template>
  <div
    class="admin-shell"
    :class="{ 'is-rail-collapsed': railCollapsed && isDesktop }"
    data-testid="admin-shell"
    :data-layout="layout"
  >
    <SideRail
      :open="drawerOpen"
      :collapsed="railCollapsed && isDesktop"
      :active-path="route.path"
      @navigate="drawerOpen = false"
      @close="drawerOpen = false"
      @toggle-collapse="railCollapsed = !railCollapsed"
    />
    <div v-if="drawerOpen" class="rail-backdrop" data-testid="rail-backdrop" @click="drawerOpen = false"></div>

    <div class="admin-main">
      <TopBar :title="pageTitle" :crumb="crumb" @toggle-rail="drawerOpen = !drawerOpen" @logout="handleLogout" />

      <main class="admin-content" data-testid="admin-content">
        <!-- 会话失效提示：给出显式动作，而不是静默白屏 -->
        <p v-if="auth.sessionExpired" class="alert alert--error" data-testid="session-expired">
          {{ auth.error || '登录已失效，请重新登录' }}
          <span class="alert__actions">
            <RouterLink class="btn btn--sm" to="/login" data-testid="session-expired-login">重新登录</RouterLink>
          </span>
        </p>
        <p v-else-if="auth.reauthError" class="alert alert--error" data-testid="reauth-error-banner">
          {{ auth.reauthError }}
        </p>

        <RouterView v-slot="{ Component, route: current }">
          <!--
            必须保留 CSS 过渡（不要把 Transition 的 css 选项设为 false）：out-in 模式靠
            离场结束后的 afterLeave 回调触发重新渲染，而 CSS 过渡是异步结束的。
            若关掉 CSS 又不给 @enter/@leave 钩子，Vue 会同步判定离场完成，
            afterLeave 里的 instance.update() 会在旧子树正卸载时重入 patch，
            抛 "Cannot read properties of null (reading 'parentNode')"，
            BaseTransition 的 isLeaving 卡在 true，此后每次切换都只剩注释占位（内容全空）。
          -->
          <Transition name="page" mode="out-in">
            <component :is="Component" :key="current.fullPath" />
          </Transition>
        </RouterView>
      </main>
    </div>

    <!-- 敏感操作的重新验证弹窗：由 auth store 的 ensureReauth 打开 -->
    <ReauthDialog
      v-if="auth.reauthActive"
      :loading="auth.reauthLoading"
      @submit="submitReauth"
      @cancel="auth.cancelReauth()"
    />
  </div>
</template>
