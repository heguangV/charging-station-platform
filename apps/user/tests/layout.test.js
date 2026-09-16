import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { nextTick } from 'vue'
import { existsSync, readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { createMemoryHistory, createRouter } from 'vue-router'
import AppShell from '../src/components/AppShell.vue'
import BottomNav, { NAV_ITEMS } from '../src/components/BottomNav.vue'
import appRouter from '../src/router'

// 直接读取全局样式源码：vitest 默认不处理 CSS，?raw 会得到空字符串。
const styleCss = readProjectFile('src/style.css')

/** 读取仓库内源文件：jsdom 环境下 import.meta.url 不是 file://，因此基于 cwd 定位。 */
function readProjectFile(relativePath) {
  const direct = resolve(process.cwd(), relativePath)
  const fallback = resolve(process.cwd(), 'apps/user', relativePath)
  return readFileSync(existsSync(direct) ? direct : fallback, 'utf8')
}

const DESKTOP_QUERY = '(min-width: 901px)'
const DESTINATIONS = ['附近', '充电', 'AI 助手', '订单', '我的']

/** jsdom 不实现 matchMedia，这里提供可手动触发 change 的替身来验证布局切换。 */
function stubMatchMedia(initialMatches) {
  const listeners = new Set()
  const mediaQueryList = {
    matches: initialMatches,
    media: DESKTOP_QUERY,
    addEventListener: (_type, listener) => listeners.add(listener),
    removeEventListener: (_type, listener) => listeners.delete(listener),
    addListener: listener => listeners.add(listener),
    removeListener: listener => listeners.delete(listener)
  }
  window.matchMedia = vi.fn(() => mediaQueryList)
  return {
    queries: () => window.matchMedia.mock.calls.length,
    emit(matches) {
      mediaQueryList.matches = matches
      for (const listener of listeners) listener({ matches })
    }
  }
}

async function mountShell() {
  const router = createRouter({
    history: createMemoryHistory(),
    routes: [
      { path: '/', component: { template: '<div data-testid="page-stub" />' } },
      { path: '/:pathMatch(.*)*', component: { template: '<div />' } }
    ]
  })
  await router.push('/')
  await router.isReady()
  const wrapper = mount(AppShell, { global: { plugins: [router] } })
  return { wrapper, router }
}

beforeEach(() => {
  setActivePinia(createPinia())
})

afterEach(() => {
  delete window.matchMedia
  vi.unstubAllGlobals()
})

describe('响应式布局：一套页面，两种呈现', () => {
  it('同一份 DOM 中同时存在侧边栏与底部导航（仅由 CSS 决定谁可见）', async () => {
    stubMatchMedia(false)
    const { wrapper } = await mountShell()

    expect(wrapper.find('[data-testid="app-sidebar"]').exists()).toBe(true)
    expect(wrapper.find('[data-testid="bottom-nav"]').exists()).toBe(true)
    expect(wrapper.get('[data-testid="app-shell"]').attributes('data-layout')).toBe('mobile')
    expect(window.matchMedia).toHaveBeenCalledWith(DESKTOP_QUERY)
  })

  it('matchMedia 命中桌面断点时切换为 desktop 布局，并监听后续变化', async () => {
    const media = stubMatchMedia(true)
    const { wrapper } = await mountShell()

    expect(wrapper.get('[data-testid="app-shell"]').attributes('data-layout')).toBe('desktop')

    media.emit(false)
    await nextTick()
    expect(wrapper.get('[data-testid="app-shell"]').attributes('data-layout')).toBe('mobile')

    media.emit(true)
    await nextTick()
    expect(wrapper.get('[data-testid="app-shell"]').attributes('data-layout')).toBe('desktop')
  })

  it('底部导航与侧边栏使用同一组 5 个目的地', async () => {
    stubMatchMedia(false)
    const { wrapper } = await mountShell()

    const bottomItems = wrapper.findAll('[data-testid="bottom-nav"] a')
    expect(bottomItems).toHaveLength(5)
    const bottomLabels = wrapper.findAll('[data-testid="bottom-nav"] .bottom-nav__label').map(node => node.text())
    expect(bottomLabels).toEqual(DESTINATIONS)

    const sidebarLinks = wrapper.findAll('[data-testid="app-sidebar"] nav a')
    expect(sidebarLinks).toHaveLength(5)
    const sidebarLabels = wrapper.findAll('[data-testid="app-sidebar"] .app-sidebar__link span:nth-child(2)').map(node => node.text())
    expect(sidebarLabels).toEqual(DESTINATIONS)

    expect(NAV_ITEMS.map(item => item.to)).toEqual(['/', '/charging', '/agent', '/orders', '/profile'])
  })

  it('BottomNav 单独挂载时也渲染 5 个目的地并标记当前路由', async () => {
    const router = createRouter({
      history: createMemoryHistory(),
      routes: [
        { path: '/', component: { template: '<div />' } },
        { path: '/agent', component: { template: '<div />' } },
        { path: '/:pathMatch(.*)*', component: { template: '<div />' } }
      ]
    })
    await router.push('/agent')
    await router.isReady()

    const wrapper = mount(BottomNav, { global: { plugins: [router] } })
    expect(wrapper.findAll('a')).toHaveLength(5)
    expect(wrapper.get('[data-testid="bottom-nav-agent"]').classes()).toContain('is-active')
    expect(wrapper.get('[data-testid="bottom-nav-agent"]').attributes('aria-current')).toBe('page')
    expect(wrapper.get('[data-testid="bottom-nav-home"]').classes()).not.toContain('is-active')
  })

  it('CSS 中定义了 901px/900px 断点，桌面显示侧边栏并隐藏底部导航', () => {
    const desktopStart = styleCss.indexOf('@media (min-width: 901px)')
    const mobileStart = styleCss.indexOf('@media (max-width: 900px)')
    expect(desktopStart).toBeGreaterThan(-1)
    expect(mobileStart).toBeGreaterThan(desktopStart)

    const desktopBlock = styleCss.slice(desktopStart, mobileStart)
    expect(desktopBlock).toMatch(/\.app-sidebar\s*\{[^}]*display:\s*flex/)
    expect(desktopBlock).toMatch(/\.bottom-nav\s*\{[^}]*display:\s*none/)
    expect(desktopBlock).toMatch(/\.split-layout\s*\{[^}]*grid-template-columns/)

    // 移动端为默认样式：侧边栏隐藏、底部导航固定显示。
    const baseBlock = styleCss.slice(0, desktopStart)
    expect(baseBlock).toMatch(/\.app-sidebar\s*\{\s*display:\s*none/)
    expect(baseBlock).toMatch(/\.bottom-nav\s*\{[^}]*position:\s*fixed/)
    expect(baseBlock).toMatch(/\.bottom-nav\s*\{[^}]*display:\s*flex/)
  })

  it('路由只有一套页面集合，没有移动端专用路由', () => {
    const paths = appRouter.getRoutes().map(route => route.path)
    for (const path of ['/', '/stations/:stationId', '/charging', '/agent', '/orders', '/profile']) {
      expect(paths).toContain(path)
    }
    expect(paths.some(path => /mobile|m\/|wap|h5/i.test(path))).toBe(false)
    expect(paths).toContain('/:pathMatch(.*)*')

    const names = appRouter.getRoutes().map(route => route.name).filter(Boolean)
    expect(names.sort()).toEqual(['agent', 'charging', 'home', 'orders', 'profile', 'station'])
  })
})
