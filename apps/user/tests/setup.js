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
