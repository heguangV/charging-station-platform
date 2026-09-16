/**
 * 数据更新高亮：值发生变化时，给承载该值的元素加一次短暂的 .value-flash 动画
 * （见 src/styles/motion.css），让“刷新/切换”有肉眼可察觉的反馈。
 *
 * 关键约束：**只改 class，绝不改文本**。因此站点距离、价格、空闲数、路线时长等
 * 被测试逐字断言的值不会出现动画中间态，数值也不会因为动画而抖动。
 */

import { onBeforeUnmount, ref, watch } from 'vue'
import { prefersReducedMotion } from './useReducedMotion'

/** 与 motion.css 的 --ncs-dur-4 对齐。 */
const FLASH_MS = 720

/**
 * @param {() => unknown} source 需要监听的响应式取值。
 * @returns {{ flashing: import('vue').Ref<boolean> }} 变化的瞬间为 true（约 720ms）。
 */
export function useValueFlash(source) {
  const flashing = ref(false)
  let timer = null

  function stop() {
    if (timer) clearTimeout(timer)
    timer = null
    flashing.value = false
  }

  watch(source, (next, previous) => {
    // 首次渲染（previous === undefined）不闪烁：那是页面进入，不是数据更新。
    if (previous === undefined || Object.is(next, previous)) return
    if (prefersReducedMotion()) return

    stop()
    flashing.value = false
    // 强制下一帧重新触发动画：先移除 class，再加回来。
    requestAnimationFrame(() => {
      flashing.value = true
      timer = setTimeout(() => {
        flashing.value = false
        timer = null
      }, FLASH_MS)
    })
  })

  onBeforeUnmount(stop)

  return { flashing }
}
