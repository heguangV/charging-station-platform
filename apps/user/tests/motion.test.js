import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { defineComponent, h, nextTick, ref } from 'vue'
import { mount } from '@vue/test-utils'
import { existsSync, readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { attachReveal, staggerStyle, REVEAL_SETTLE_MS } from '../src/composables/useReveal'
import {
  prefersReducedMotion,
  resetReducedMotionState
} from '../src/composables/useReducedMotion'
import { useValueFlash } from '../src/composables/useValueFlash'
import { vReveal } from '../src/directives/reveal'

/** 基于 cwd 定位源文件：jsdom 下 import.meta.url 不是 file://。 */
function readProjectFile(relativePath) {
  const direct = resolve(process.cwd(), relativePath)
  const fallback = resolve(process.cwd(), 'apps/user', relativePath)
  return readFileSync(existsSync(direct) ? direct : fallback, 'utf8')
}

const motionCss = readProjectFile('src/styles/motion.css')
const styleCss = readProjectFile('src/style.css')
const mainJs = readProjectFile('src/main.js')

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
  // 让 rAF 同步执行，使“数据更新高亮”在断言前就进入可观察状态。
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
  it('staggerStyle 把序号换算成 CSS 变量与延迟，并封顶 12 项', () => {
    expect(staggerStyle(0)).toEqual({ '--stagger-index': '0', animationDelay: '0ms' })
    expect(staggerStyle(3, 50)).toEqual({ '--stagger-index': '3', animationDelay: '150ms' })
    expect(staggerStyle(99)['--stagger-index']).toBe('12')
    expect(staggerStyle(undefined)).toEqual({ '--stagger-index': '0', animationDelay: '0ms' })
  })

  it('进入视口后加上 is-visible，并在过渡结束后加上 is-settled 且停止观察', () => {
    const observers = stubIntersectionObserver()
    const element = document.createElement('div')

    const cleanup = attachReveal(element)
    expect(element.classList.contains('reveal')).toBe(true)
    expect(element.classList.contains('is-visible')).toBe(false)
    expect(observers).toHaveLength(1)
    expect(observers[0].observed).toEqual([element])

    observers[0].trigger(true)
    expect(element.classList.contains('is-visible')).toBe(true)
    expect(observers[0].disconnected).toBe(true)
    expect(element.classList.contains('is-settled')).toBe(false)

    vi.advanceTimersByTime(REVEAL_SETTLE_MS + 20)
    expect(element.classList.contains('is-settled')).toBe(true)

    cleanup()
    expect(observers[0].disconnected).toBe(true)
  })

  it('元素尚未进入视口时保持不可见，不误播放', () => {
    const observers = stubIntersectionObserver()
    const element = document.createElement('div')
    attachReveal(element)

    observers[0].trigger(false)
    expect(element.classList.contains('is-visible')).toBe(false)
    expect(observers[0].disconnected).toBe(false)
  })

  it('按序号写入 --reveal-delay', () => {
    stubIntersectionObserver()
    const element = document.createElement('div')
    attachReveal(element, { index: 4, step: 30 })
    expect(element.style.getPropertyValue('--reveal-delay')).toBe('120ms')
  })

  it('缺少 IntersectionObserver 时直接给终态，不会留下不可见内容', () => {
    // jsdom 默认没有 IntersectionObserver，这里刻意不注入替身。
    const element = document.createElement('div')
    attachReveal(element)
    expect(element.classList.contains('reveal')).toBe(true)
    expect(element.classList.contains('is-visible')).toBe(true)
    expect(element.classList.contains('is-settled')).toBe(true)
  })

  it('命中“减弱动态效果”时不做观察，直接给终态', () => {
    const observers = stubIntersectionObserver()
    stubMatchMedia(true)
    expect(prefersReducedMotion()).toBe(true)

    const element = document.createElement('div')
    attachReveal(element)
    expect(observers).toHaveLength(0)
    expect(element.classList.contains('is-visible')).toBe(true)
  })
})

describe('v-reveal 指令', () => {
  it('挂载时给元素加 reveal 类，卸载时释放观察器', async () => {
    const observers = stubIntersectionObserver()
    const wrapper = mount(
      { template: '<div v-reveal id="target"></div>' },
      { global: { directives: { reveal: vReveal } } }
    )

    const element = wrapper.get('#target').element
    expect(element.classList.contains('reveal')).toBe(true)
    expect(observers).toHaveLength(1)

    observers[0].trigger(true)
    await nextTick()
    expect(element.classList.contains('is-visible')).toBe(true)

    wrapper.unmount()
    expect(observers.every(observer => observer.disconnected)).toBe(true)
  })

  it('v-reveal 传数字时按序号错开延迟', () => {
    stubIntersectionObserver()
    const wrapper = mount(
      { template: '<div v-reveal="3" id="target"></div>' },
      { global: { directives: { reveal: vReveal } } }
    )
    expect(wrapper.get('#target').element.style.getPropertyValue('--reveal-delay')).toBe('135ms')
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
    const wrapper = mount(Host)
    return { wrapper, value }
  }

  it('首次渲染不闪烁，值变化后才加 value-flash', async () => {
    const { wrapper, value } = mountFlash('1.35')
    expect(wrapper.get('#value').classes()).not.toContain('value-flash')

    value.value = '1.45'
    await nextTick()
    expect(wrapper.get('#value').classes()).toContain('value-flash')
    // 文本始终是最终值，动画不制造中间态
    expect(wrapper.get('#value').text()).toBe('1.45')

    vi.advanceTimersByTime(1000)
    await nextTick()
    expect(wrapper.get('#value').classes()).not.toContain('value-flash')
  })

  it('值未变化时不闪烁', async () => {
    const { wrapper, value } = mountFlash('2.30')
    value.value = '2.30'
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
  it('motion.css 定义了所需的过渡与关键帧', () => {
    for (const keyframe of ['ncs-rise-in', 'ncs-fade-in', 'ncs-shimmer', 'ncs-value-flash', 'ncs-typing']) {
      expect(motionCss).toContain(`@keyframes ${keyframe}`)
    }
    for (const hook of ['.reveal', '.skeleton', '.value-flash', '.stagger-item', '.page-enter-from', '.list-move', '.typing-dot']) {
      expect(motionCss).toContain(hook)
    }
  })

  it('全局提供 prefers-reduced-motion 降级，禁用动画与平滑滚动', () => {
    const guard = motionCss.slice(motionCss.indexOf('@media (prefers-reduced-motion: reduce)'))
    expect(guard).toContain('animation-duration: 0.001ms !important')
    expect(guard).toContain('transition-duration: 0.001ms !important')
    expect(guard).toContain('scroll-behavior: auto !important')
  })

  it('style.css 保留 900/901 断点结构与原有变量名，组件样式不会失去依托', () => {
    expect(styleCss).toContain('@media (min-width: 901px)')
    expect(styleCss).toContain('@media (max-width: 900px)')
    for (const token of ['--ncs-brand', '--ncs-surface', '--ncs-border', '--ncs-muted', '--ncs-radius', '--ncs-shadow']) {
      expect(styleCss).toContain(token)
    }
    // 顶部栏改为毛玻璃后仍必须保留发丝分隔线，避免与内容粘连
    expect(styleCss).toMatch(/\.app-topbar\s*\{[^}]*backdrop-filter/)
  })

  it('main.js 注册全局 v-reveal 指令并加载动效层', () => {
    expect(mainJs).toContain("app.directive('reveal', vReveal)")
    expect(mainJs).toContain("import './styles/motion.css'")
  })
})
