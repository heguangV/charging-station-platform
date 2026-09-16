/**
 * v-reveal 指令：滚动进入视口时播放一次“上浮 + 淡入”，不改变 DOM 结构，因此可以安全地
 * 加在任何既有元素上（panel、卡片、列表容器），既有的 data-testid 与样式选择器都不受影响。
 *
 * 用法：
 *   <div v-reveal>…</div>              立即播放
 *   <div v-reveal="{ delay: 80 }">…</div>
 *   <li v-for="(item, i) in list" v-reveal="i">…</li>   按序号自动错开
 */

import { attachReveal } from '@/composables/useReveal'

/** 保存每个元素对应的清理函数，卸载时统一释放。 */
const cleanups = new WeakMap()

export const vReveal = {
  mounted(element, binding) {
    const value = binding.value
    const options = typeof value === 'number' ? { index: value } : (value || {})
    cleanups.set(element, attachReveal(element, options))
  },

  unmounted(element) {
    const cleanup = cleanups.get(element)
    if (cleanup) cleanup()
    cleanups.delete(element)
  }
}

export default vReveal
