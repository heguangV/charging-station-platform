/**
 * 滚动进入动效的共享实现。
 *
 * 供 v-reveal 指令使用：元素进入视口时加 .is-visible，配合 src/styles/motion.css 的 .reveal
 * 做“上浮 + 淡入”。纯视觉，不改动文案、数值与 data-testid，也不改变 DOM 结构。
 *
 * 设计要点：
 * - 用 IntersectionObserver，不用 scroll 事件，划动页面时不产生每帧回调；
 * - 只观察一次（进入即取消观察），避免来回滚动反复播放；
 * - 命中 prefers-reduced-motion 或环境不支持 IntersectionObserver（如 jsdom）时
 *   直接给终态，绝不出现“内容永远不可见”的失败模式。
 */

import { prefersReducedMotion } from './useReducedMotion'

/** 元素距视口底部还有 8% 时开始播放，划动时更跟手。 */
const ROOT_MARGIN = '0px 0px -8% 0px'
/** 过渡结束后的清理延时，略大于 motion.css 的 --ncs-dur-3。 */
export const REVEAL_SETTLE_MS = 520

/**
 * 给元素挂上滚动进入动效。
 * @param {HTMLElement} element 目标元素
 * @param {{ delay?: number, step?: number, index?: number }} [options]
 *        delay：固定延迟（毫秒）；index/step：按列表序号错开。
 * @returns {() => void} 清理函数（取消观察 / 清理定时器）
 */
export function attachReveal(element, options = {}) {
  if (!element || !element.classList) return () => {}

  element.classList.add('reveal')
  const delay = Number(options.delay ?? (Number(options.index ?? 0) || 0) * (options.step ?? 45))
  if (delay > 0) element.style.setProperty('--reveal-delay', `${delay}ms`)

  const finish = () => {
    element.classList.add('is-visible')
    // 过渡结束后撤掉 will-change，避免长期占用合成层。
    const timer = setTimeout(() => element.classList.add('is-settled'), REVEAL_SETTLE_MS)
    return () => clearTimeout(timer)
  }

  if (prefersReducedMotion() || typeof IntersectionObserver !== 'function') {
    const clear = finish()
    element.classList.add('is-settled')
    return clear
  }

  let clearSettle = null
  const observer = new IntersectionObserver(
    entries => {
      for (const entry of entries) {
        if (!entry.isIntersecting) continue
        observer.disconnect()
        clearSettle = finish()
      }
    },
    { rootMargin: ROOT_MARGIN, threshold: 0.08 }
  )
  observer.observe(element)

  return () => {
    observer.disconnect()
    if (clearSettle) clearSettle()
  }
}

/**
 * 列表逐项错开的样式：把序号换算成 CSS 变量，由 .stagger-item 的 animation-delay 消费。
 * 上限 12 项，避免长列表末尾等待过久。
 */
export function staggerStyle(index, step = 45) {
  const capped = Math.min(Number(index) || 0, 12)
  return { '--stagger-index': String(capped), animationDelay: `${capped * step}ms` }
}
