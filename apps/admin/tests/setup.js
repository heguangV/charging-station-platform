/**
 * 全局测试引导。
 * v-reveal 在 main.js 里通过 app.directive 注册；@vue/test-utils 的 mount 不共享该应用实例，
 * 因此这里把同一个指令挂到测试工具链的全局配置上，保证视图在测试中与在运行时行为一致，
 * 也避免出现 "Failed to resolve directive: reveal" 之类的噪声告警。
 */

import { config } from '@vue/test-utils'
import { vReveal } from '../src/directives/reveal'

config.global.directives = { ...(config.global.directives || {}), reveal: vReveal }

// jsdom 不实现 IntersectionObserver，指令与骨架屏在缺失时应走“直接到终态”的降级分支；
// 这里刻意不补实现，以便测试同时覆盖该降级路径。

/**
 * 浏览器长期存储替身。
 *
 * Node 22+ 在全局声明了 localStorage，但未启用 --localstorage-file 时其值为 undefined，
 * 该同名全局会遮挡 jsdom 提供的实现，导致“本地存储未被写入”这条断言无法观察。
 * 这里补一个最小实现：测试用它验证令牌只进 sessionStorage，永不进 localStorage（安全基线）。
 */
function createMemoryStorage() {
  const map = new Map()
  return {
    get length() {
      return map.size
    },
    key: index => Array.from(map.keys())[index] ?? null,
    getItem: key => (map.has(String(key)) ? map.get(String(key)) : null),
    setItem: (key, value) => {
      map.set(String(key), String(value))
    },
    removeItem: key => {
      map.delete(String(key))
    },
    clear: () => {
      map.clear()
    }
  }
}

try {
  const browserLocalStorage = createMemoryStorage()
  Object.defineProperty(globalThis, 'localStorage', {
    value: browserLocalStorage,
    configurable: true,
    writable: true
  })
  if (typeof window !== 'undefined') {
    Object.defineProperty(window, 'localStorage', {
      value: browserLocalStorage,
      configurable: true,
      writable: true
    })
  }
} catch {
  /* 环境不允许覆盖时保持原样：相关断言会以明确失败暴露，而不是静默跳过。 */
}
