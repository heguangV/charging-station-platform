/**
 * 系统“减弱动态效果”偏好。
 * 所有 JS 驱动的动效（滚动进入、数据更新高亮）都必须先问这里，
 * 命中时直接给终态、不加任何过渡，与 src/styles/motion.css 的 CSS 降级保持一致。
 */

/** 单例订阅：整页只需要一个 matchMedia 查询，避免每个组件各建一个。 */
const REDUCE_QUERY = '(prefers-reduced-motion: reduce)'

let mediaQueryList = null
const listeners = new Set()

function ensureQuery() {
  if (mediaQueryList) return mediaQueryList
  if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') return null
  mediaQueryList = window.matchMedia(REDUCE_QUERY)
  const notify = event => {
    for (const listener of listeners) listener(event.matches)
  }
  if (typeof mediaQueryList.addEventListener === 'function') mediaQueryList.addEventListener('change', notify)
  else if (typeof mediaQueryList.addListener === 'function') mediaQueryList.addListener(notify)
  return mediaQueryList
}

/** 当前是否为“减弱动态效果”。无 matchMedia（如 jsdom 未打桩）时按 false 处理。 */
export function prefersReducedMotion() {
  const query = ensureQuery()
  return query ? query.matches === true : false
}

/** 订阅偏好变化；返回取消订阅函数。 */
export function onReducedMotionChange(listener) {
  ensureQuery()
  listeners.add(listener)
  return () => listeners.delete(listener)
}

/** 组合式封装：供组件在 setup 中使用。 */
export function useReducedMotion() {
  return { prefersReducedMotion, onReducedMotionChange }
}

/** 仅测试使用：清空单例，隔离用例之间的 matchMedia 替身。 */
export function resetReducedMotionState() {
  mediaQueryList = null
  listeners.clear()
}
