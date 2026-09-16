import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { defineComponent } from 'vue'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { createMemoryHistory, createRouter } from 'vue-router'
import AdminShell from '../src/components/AdminShell.vue'
import { NAV_ITEMS } from '../src/components/SideRail.vue'
import { authGuard, routes } from '../src/router/index.js'
import { clearAccessToken } from '../src/api/http'
import { useAuthStore } from '../src/stores/auth'
import { flush, installFetch, okResponse, settle } from './helpers'

const EXPECTED_DESTINATIONS = [
  { key: 'overview', to: '/', label: '总览' },
  { key: 'stations', to: '/stations', label: '站点' },
  { key: 'chargers', to: '/chargers', label: '充电桩' },
  { key: 'users', to: '/users', label: '用户' },
  { key: 'flows', to: '/flows', label: '活动流程' },
  { key: 'appeals', to: '/appeals', label: '申诉管理' },
  { key: 'accounts', to: '/accounts', label: '管理员' },
  { key: 'ops', to: '/ops', label: '运维' }
]

const Stub = defineComponent({ name: 'StubPage', template: '<div data-testid="page-stub">页面占位</div>' })

/** 用真实路由表构建“组件已替换”的测试路由，结构、meta 与守卫完全一致。 */
function buildStubRoutes() {
  return routes.map(route =>
    route.children
      ? { ...route, component: Stub, children: route.children.map(child => ({ ...child, component: Stub })) }
      : { ...route, component: Stub }
  )
}

function stubMatchMedia(matches) {
  window.matchMedia = vi.fn(() => ({
    matches,
    media: '(min-width: 1024px)',
    addEventListener: () => {},
    removeEventListener: () => {},
    addListener: () => {},
    removeListener: () => {}
  }))
}

async function mountShell({ desktop = true, authenticated = true } = {}) {
  stubMatchMedia(desktop)
  const pinia = createPinia()
  setActivePinia(pinia)
  const auth = useAuthStore()
  if (authenticated) {
    auth.token = 'admin-token'
    auth.admin = { id: 1, username: 'admin', roles: ['OWNER'], status: 1, mustChangePassword: false, version: 1 }
  }
  // 用真实路由表（组件替换为占位）挂载，保证侧栏链接、meta 与守卫都和线上一致
  const router = createRouter({ history: createMemoryHistory(), routes: buildStubRoutes() })
  router.beforeEach(authGuard)
  await router.push('/')
  await router.isReady()
  // stubs.transition=false 关掉 test-utils 对 <Transition> 的替身，让路由切换走真实过渡逻辑
  const wrapper = mount(AdminShell, { global: { plugins: [pinia, router], stubs: { transition: false } } })
  await wrapper.vm.$nextTick()
  return { wrapper, router, auth, pinia }
}

/** 等待若干真实帧：CSS 过渡在下一帧推进，await nextTick() 等不到它结束。 */
async function frames(times = 6) {
  for (let index = 0; index < times; index += 1) {
    await new Promise(resolve => requestAnimationFrame(resolve))
  }
}

beforeEach(() => {
  sessionStorage.clear()
  localStorage.clear()
})

afterEach(() => {
  delete window.matchMedia
  vi.unstubAllGlobals()
})

describe('唯一路由表', () => {
  it('只有一套页面：登录 + 一个 Shell + 8 个目的地 + 通配回落', () => {
    expect(routes).toHaveLength(3)

    const login = routes.find(route => route.path === '/login')
    expect(login.meta.public).toBe(true)

    const shell = routes.find(route => Array.isArray(route.children))
    expect(shell.path).toBe('/')
    expect(shell.children).toHaveLength(8)
    expect(shell.children.map(child => child.path)).toEqual(['', 'stations', 'chargers', 'users', 'flows', 'appeals', 'accounts', 'ops'])
    expect(routes[routes.length - 1].path).toBe('/:pathMatch(.*)*')
    expect(routes[routes.length - 1].redirect).toBe('/')
  })

  it('不存在移动端专用路由、UA 分流或第二套页面', () => {
    const serialized = JSON.stringify(routes)
    expect(serialized).not.toMatch(/mobile/i)
    expect(serialized).not.toMatch(/userAgent|isMobile|touch/i)
    // 每个目的地只有一个路由记录
    const paths = routes
      .filter(route => Array.isArray(route.children))
      .flatMap(route => route.children.map(child => `/${child.path}`.replace('//', '/')))
    expect(new Set(paths).size).toBe(paths.length)
  })

  it('侧栏目的地与路由表一一对应', () => {
    expect(NAV_ITEMS).toHaveLength(8)
    expect(NAV_ITEMS.map(item => ({ key: item.key, to: item.to, label: item.label }))).toEqual(EXPECTED_DESTINATIONS)

    const shell = routes.find(route => Array.isArray(route.children))
    const childPaths = shell.children.map(child => `/${child.path}`.replace('//', '/'))
    expect(childPaths).toEqual(NAV_ITEMS.map(item => item.to))
  })
})

describe('页面骨架与侧栏', () => {
  it('渲染 8 个导航目的地并标记当前项', async () => {
    const { wrapper } = await mountShell()
    expect(wrapper.get('[data-testid="admin-shell"]').attributes('data-layout')).toBe('desktop')
    expect(wrapper.find('[data-testid="side-rail"]').exists()).toBe(true)

    for (const item of EXPECTED_DESTINATIONS) {
      expect(wrapper.find(`[data-testid="rail-${item.key}"]`).exists()).toBe(true)
    }
    expect(wrapper.findAll('.rail-item')).toHaveLength(8)

    // 当前路由为 '/'，只有“总览”处于选中态
    expect(wrapper.get('[data-testid="rail-overview"]').classes()).toContain('is-active')
    expect(wrapper.get('[data-testid="rail-stations"]').classes()).not.toContain('is-active')
    expect(wrapper.get('[data-testid="top-bar"]').exists()).toBe(true)
    expect(wrapper.get('[data-testid="page-title"]').text()).toBe('运营总览')
    expect(wrapper.get('[data-testid="admin-identity"]').text()).toContain('admin')
  })

  it('路由切换过渡使用 <Transition name="page" mode="out-in">', () => {
    const source = readFileSync(resolve(process.cwd(), 'src/components/AdminShell.vue'), 'utf8')
    expect(source).toContain('<Transition name="page" mode="out-in">')
    expect(source).toContain('<RouterView v-slot="{ Component, route: current }">')
    // 关掉 CSS 过渡会让离场同步结束，out-in 的重渲染重入 patch 后内容会全部消失。
    expect(source).not.toContain(':css="false"')
  })

  it('侧栏切换路由后新页面必须重新出现，而不是只剩空内容区', async () => {
    const { wrapper, router } = await mountShell()
    expect(wrapper.get('[data-testid="admin-content"]').find('[data-testid="page-stub"]').exists()).toBe(true)

    for (const destination of ['/stations', '/chargers', '/accounts', '/']) {
      await router.push(destination)
      await frames()
      expect(router.currentRoute.value.path).toBe(destination)
      expect(wrapper.get('[data-testid="admin-content"]').find('[data-testid="page-stub"]').exists()).toBe(true)
    }
  })

  it('窄屏（<1024px）侧栏收为抽屉，可由顶栏按钮开合并由遮罩关闭', async () => {
    const { wrapper } = await mountShell({ desktop: false })
    expect(wrapper.get('[data-testid="admin-shell"]').attributes('data-layout')).toBe('narrow')
    expect(wrapper.find('[data-testid="rail-backdrop"]').exists()).toBe(false)

    await wrapper.get('[data-testid="rail-open"]').trigger('click')
    expect(wrapper.get('[data-testid="side-rail"]').classes()).toContain('is-open')
    expect(wrapper.find('[data-testid="rail-backdrop"]').exists()).toBe(true)

    await wrapper.get('[data-testid="rail-backdrop"]').trigger('click')
    expect(wrapper.get('[data-testid="side-rail"]').classes()).not.toContain('is-open')
  })

  it('宽屏侧栏可折叠为图标态', async () => {
    const { wrapper } = await mountShell({ desktop: true })
    expect(wrapper.get('[data-testid="admin-shell"]').classes()).not.toContain('is-rail-collapsed')
    await wrapper.get('[data-testid="rail-collapse"]').trigger('click')
    expect(wrapper.get('[data-testid="admin-shell"]').classes()).toContain('is-rail-collapsed')
    expect(wrapper.get('[data-testid="side-rail"]').classes()).toContain('is-collapsed')
  })

  it('退出登录清理会话并回到登录页', async () => {
    const harness = installFetch([okResponse({})])
    const { wrapper, router, auth } = await mountShell()
    await wrapper.get('[data-testid="admin-logout"]').trigger('click')
    await flush(6)
    await settle(2)

    expect(harness.indexOf('POST', '/auth/logout')).toBe(0)
    expect(auth.isLoggedIn).toBe(false)
    expect(auth.token).toBeNull()
    expect(sessionStorage.getItem('ncs.admin.accessToken')).toBeNull()
    expect(router.currentRoute.value.path).toBe('/login')
    vi.unstubAllGlobals()
  })
})

describe('路由守卫', () => {
  it('未登录访问受保护页面时跳转登录页并携带原地址', async () => {
    clearAccessToken()
    const pinia = createPinia()
    setActivePinia(pinia)
    const router = createRouter({ history: createMemoryHistory(), routes: buildStubRoutes() })
    router.beforeEach(authGuard)

    await router.push('/stations')
    expect(router.currentRoute.value.path).toBe('/login')
    expect(router.currentRoute.value.query.redirect).toBe('/stations')

    await router.push('/')
    expect(router.currentRoute.value.path).toBe('/login')
    expect(router.currentRoute.value.query.redirect).toBeUndefined()
  })

  it('已登录访问 /login 时回到总览', async () => {
    const pinia = createPinia()
    setActivePinia(pinia)
    const auth = useAuthStore()
    auth.token = 'admin-token'
    const router = createRouter({ history: createMemoryHistory(), routes: buildStubRoutes() })
    router.beforeEach(authGuard)

    await router.push('/login')
    expect(router.currentRoute.value.path).toBe('/')
  })

  it('已登录可以进入受保护页面', async () => {
    const pinia = createPinia()
    setActivePinia(pinia)
    const auth = useAuthStore()
    auth.token = 'admin-token'
    const router = createRouter({ history: createMemoryHistory(), routes: buildStubRoutes() })
    router.beforeEach(authGuard)

    await router.push('/ops')
    expect(router.currentRoute.value.path).toBe('/ops')
  })
})
