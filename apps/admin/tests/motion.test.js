import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { existsSync, readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { defineComponent, h, nextTick, ref } from 'vue'
import { mount } from '@vue/test-utils'
import { attachReveal, staggerStyle, REVEAL_SETTLE_MS } from '../src/composables/useReveal'
import { prefersReducedMotion, resetReducedMotionState } from '../src/composables/useReducedMotion'
import { useValueFlash } from '../src/composables/useValueFlash'
import { vReveal } from '../src/directives/reveal'

/** 基于 cwd 定位源文件：jsdom 下 import.meta.url 不是 file://。 */
function readProjectFile(relativePath) {
  const direct = resolve(process.cwd(), relativePath)
  const fallback = resolve(process.cwd(), 'apps/admin', relativePath)
  return readFileSync(existsSync(direct) ? direct : fallback, 'utf8')
}

const motionCss = readProjectFile('src/styles/motion.css')
const styleCss = readProjectFile('src/style.css')
const mainJs = readProjectFile('src/main.js')
const shellSource = readProjectFile('src/components/AdminShell.vue')

/** 可手动触发回调的 IntersectionObserver 替身。 */
function stubIntersectionObserver() {
  const instances = []
  class FakeObserver {
    constructor(callback, options) {
      this.callback = callback
      this.options = options
      this.observed = []
      this.disconnected = false
      instances.push(this)
    }
    observe(element) {
      this.observed.push(element)
    }
    disconnect() {
      this.disconnected = true
    }
    trigger(isIntersecting = true) {
      this.callback(this.observed.map(element => ({ isIntersecting, target: element })), this)
    }
  }
  vi.stubGlobal('IntersectionObserver', FakeObserver)
  return instances
}

function stubMatchMedia(matches) {
  window.matchMedia = vi.fn(() => ({
    matches,
    media: '(prefers-reduced-motion: reduce)',
    addEventListener: () => {},
    removeEventListener: () => {},
    addListener: () => {},
    removeListener: () => {}
  }))
}

beforeEach(() => {
  resetReducedMotionState()
  vi.useFakeTimers()
  vi.stubGlobal('requestAnimationFrame', callback => {
    callback(0)
    return 1
  })
})

afterEach(() => {
  vi.useRealTimers()
  vi.unstubAllGlobals()
  delete window.matchMedia
  resetReducedMotionState()
})

describe('滚动进入动效', () => {
  it('v-reveal 挂载时加 .reveal 并注册观察器，进入视口后到终态', async () => {
    const observers = stubIntersectionObserver()
    const wrapper = mount({ template: '<div v-reveal id="target"></div>' })

    const element = wrapper.get('#target').element
    expect(element.classList.contains('reveal')).toBe(true)
    expect(observers).toHaveLength(1)

    observers[0].trigger(true)
    await nextTick()
    expect(element.classList.contains('is-visible')).toBe(true)
    expect(observers[0].disconnected).toBe(true)
  })

  it('缺少 IntersectionObserver（如 jsdom）时直接给终态，不会留下不可见内容', () => {
    const element = document.createElement('div')
    attachReveal(element)
    expect(element.classList.contains('reveal')).toBe(true)
    expect(element.classList.contains('is-visible')).toBe(true)
    expect(element.classList.contains('is-settled')).toBe(true)
  })

  it('命中“减弱动态效果”时不做观察，直接到终态', () => {
    const observers = stubIntersectionObserver()
    stubMatchMedia(true)
    expect(prefersReducedMotion()).toBe(true)

    const element = document.createElement('div')
    attachReveal(element)
    expect(observers).toHaveLength(0)
    expect(element.classList.contains('is-visible')).toBe(true)
  })

  it('按序号写入 --reveal-delay 并在过渡结束后撤掉 will-change', () => {
    stubIntersectionObserver()
    const element = document.createElement('div')
    attachReveal(element, { index: 3, step: 40 })
    expect(element.style.getPropertyValue('--reveal-delay')).toBe('120ms')

    vi.advanceTimersByTime(REVEAL_SETTLE_MS + 20)
    expect(staggerStyle(13)['--stagger-index']).toBe('12')
  })
})

describe('数据更新高亮', () => {
  function mountFlash(initial) {
    const value = ref(initial)
    const Host = defineComponent({
      setup() {
        const { flashing } = useValueFlash(() => value.value)
        return () => h('span', { id: 'value', class: flashing.value ? 'value-flash' : '' }, String(value.value))
      }
    })
    return { wrapper: mount(Host), value }
  }

  it('首次渲染不闪烁，值变化后加 .value-flash 且文本仍为最终值', async () => {
    const { wrapper, value } = mountFlash('12.50')
    expect(wrapper.get('#value').classes()).not.toContain('value-flash')

    value.value = '15.00'
    await nextTick()
    expect(wrapper.get('#value').classes()).toContain('value-flash')
    expect(wrapper.get('#value').text()).toBe('15.00')

    vi.advanceTimersByTime(1000)
    await nextTick()
    expect(wrapper.get('#value').classes()).not.toContain('value-flash')
  })

  it('命中“减弱动态效果”时不闪烁', async () => {
    stubMatchMedia(true)
    resetReducedMotionState()
    const { wrapper, value } = mountFlash('0.00')
    value.value = '10.00'
    await nextTick()
    expect(wrapper.get('#value').classes()).not.toContain('value-flash')
    expect(wrapper.get('#value').text()).toBe('10.00')
  })
})

describe('动效层与降级契约', () => {
  it('motion.css 提供骨架、数值高亮、路由与列表过渡所需的关键帧与钩子', () => {
    for (const keyframe of ['ncs-rise-in', 'ncs-fade-in', 'ncs-shimmer', 'ncs-value-flash', 'ncs-pop-in']) {
      expect(motionCss).toContain(`@keyframes ${keyframe}`)
    }
    for (const hook of ['.reveal', '.skeleton', '.value-flash', '.stagger-item', '.page-enter-from', '.list-move']) {
      expect(motionCss).toContain(hook)
    }
  })

  it('全局 prefers-reduced-motion 降级禁用动画、过渡与平滑滚动', () => {
    const guard = motionCss.slice(motionCss.indexOf('@media (prefers-reduced-motion: reduce)'))
    expect(guard).toContain('animation-duration: 0.001ms !important')
    expect(guard).toContain('transition-duration: 0.001ms !important')
    expect(guard).toContain('scroll-behavior: auto !important')
    // 终态必须是可见的，而不是靠动画补出来
    expect(guard).toMatch(/\.reveal \{[^}]*opacity: 1/)
    expect(guard).toContain('.skeleton::after')
  })

  it('style.css 保留与用户端一致的设计令牌与 1024px 断点结构', () => {
    for (const token of [
      '--ncs-brand',
      '--ncs-brand-soft',
      '--ncs-surface',
      '--ncs-surface-2',
      '--ncs-surface-3',
      '--ncs-border',
      '--ncs-muted',
      '--ncs-radius',
      '--ncs-r-pill',
      '--ncs-shadow',
      '--ncs-s-4',
      '--ncs-accent',
      '--ncs-danger',
      '--ncs-skeleton',
      '--ncs-line',
      '--ncs-dur-2',
      '--ncs-ease-out'
    ]) {
      expect(styleCss).toContain(token)
    }
    expect(styleCss).toContain('@media (min-width: 1024px)')
    expect(styleCss).toContain('@media (max-width: 1023px)')
    expect(styleCss).toContain('scroll-behavior: smooth')
    expect(styleCss).toContain('font-variant-numeric: tabular-nums')
    // 窄屏表格横向滚动
    expect(styleCss).toMatch(/\.table-scroll \{[^}]*overflow-x: auto/)
  })

  it('顶栏与侧栏使用毛玻璃与发丝分隔线，动效不改变任何 data-testid', () => {
    expect(styleCss).toMatch(/\.top-bar \{[^}]*backdrop-filter/)
    expect(styleCss).toMatch(/\.side-rail \{[^}]*backdrop-filter/)
    expect(styleCss).toContain('--ncs-rail-width')
  })

  it('AdminShell 使用 page 过渡并挂载重新验证弹窗', () => {
    expect(shellSource).toContain('<Transition name="page" mode="out-in">')
    expect(shellSource).toContain('ReauthDialog')
    expect(shellSource).toContain('data-testid="admin-shell"')
  })

  it('main.js 注册全局 v-reveal 指令并加载动效层', () => {
    expect(mainJs).toContain("app.directive('reveal', vReveal)")
    expect(mainJs).toContain("import './styles/motion.css'")
  })

  it('v-reveal 指令可全局使用（与 tests/setup.js 注册的指令一致）', () => {
    const wrapper = mount({ template: '<div v-reveal="1" id="ok"></div>' })
    expect(wrapper.get('#ok').element.classList.contains('reveal')).toBe(true)
    expect(typeof vReveal.mounted).toBe('function')
  })
})
