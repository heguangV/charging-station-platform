/**
 * ECharts 按需装配。
 *
 * 只注册管理端真正用到的图表与组件（折线/面积、柱状、环形 + 网格/提示/图例/标题/Canvas 渲染器），
 * 避免把整个 echarts 打进产物；配色与文字直接读取全局设计令牌，图表随主题变化。
 */

import { use, init, graphic } from 'echarts/core'
import { BarChart, LineChart, PieChart } from 'echarts/charts'
import { GridComponent, LegendComponent, TooltipComponent } from 'echarts/components'
import { CanvasRenderer } from 'echarts/renderers'
import { prefersReducedMotion } from '@/composables/useReducedMotion'

use([
  LineChart,
  BarChart,
  PieChart,
  GridComponent,
  TooltipComponent,
  LegendComponent,
  CanvasRenderer
])

export { init, graphic }

/** 回退配色：元素尚未挂载或 CSS 变量缺失时使用与 style.css 相同的品牌色。 */
const FALLBACK = {
  brand: '#0d7a6f',
  brandBright: '#14a192',
  accent: '#e08a1e',
  danger: '#c8382f',
  text: '#0d1622',
  text2: '#35465a',
  muted: '#6b7c8f',
  line: '#e4eaf1',
  surface: '#ffffff',
  surface2: '#f8fafc'
}

/** 读取元素上的设计令牌，得到与页面完全一致的图表配色。 */
export function chartTheme(element) {
  if (!element || typeof getComputedStyle !== 'function') return { ...FALLBACK }
  const style = getComputedStyle(element)
  const read = (name, fallback) => {
    const value = style.getPropertyValue(name)
    return value && value.trim() !== '' ? value.trim() : fallback
  }
  return {
    brand: read('--ncs-brand', FALLBACK.brand),
    brandBright: read('--ncs-brand-bright', FALLBACK.brandBright),
    accent: read('--ncs-accent', FALLBACK.accent),
    danger: read('--ncs-danger', FALLBACK.danger),
    text: read('--ncs-text', FALLBACK.text),
    text2: read('--ncs-text-2', FALLBACK.text2),
    muted: read('--ncs-muted', FALLBACK.muted),
    line: read('--ncs-line', FALLBACK.line),
    surface: read('--ncs-surface', FALLBACK.surface),
    surface2: read('--ncs-surface-2', FALLBACK.surface2)
  }
}

/** 统一的坐标轴样式：发丝网格线、无轴线，减少视觉噪声。 */
export function axisStyle(theme) {
  return {
    axisLine: { show: false },
    axisTick: { show: false },
    axisLabel: { color: theme.muted, fontSize: 11 },
    splitLine: { lineStyle: { color: theme.line, type: 'dashed' } }
  }
}

/** 统一的提示框样式。 */
export function tooltipStyle(theme) {
  return {
    backgroundColor: theme.surface,
    borderColor: theme.line,
    borderWidth: 1,
    padding: [8, 12],
    textStyle: { color: theme.text2, fontSize: 12 },
    extraCssText: 'border-radius:10px;box-shadow:0 8px 20px rgba(13,22,34,0.12);'
  }
}

/** 命中“减弱动态效果”时图表完全不做入场动画。 */
export function chartAnimation() {
  return prefersReducedMotion()
    ? { animation: false }
    : { animation: true, animationDuration: 620, animationEasing: 'cubicOut', animationDurationUpdate: 320 }
}
