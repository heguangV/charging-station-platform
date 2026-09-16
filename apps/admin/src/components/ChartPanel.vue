<script setup>
/**
 * 图表面板：ECharts 按需实例的统一容器。
 *
 * 职责：
 * 1. 容器尺寸变化时通过 ResizeObserver 重新布局（侧栏折叠、窗口缩放、抽屉开关都会触发）；
 * 2. 配置变化时增量 setOption，并在 prefers-reduced-motion 下完全关闭动画；
 * 3. 配色从设计令牌读取，跟随主题；
 * 4. 加载时显示同尺寸骨架，数据为空时显示空状态，画布本身始终保留 data-testid。
 */
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import AppSkeleton from './AppSkeleton.vue'
import { chartAnimation, chartTheme, init as initChart } from '@/charts'
import { onReducedMotionChange, prefersReducedMotion } from '@/composables/useReducedMotion'

const props = defineProps({
  title: { type: String, default: '' },
  hint: { type: String, default: '' },
  /**
   * 完整的 ECharts option（不含动画字段，动画由本组件统一注入）。
   * 也接受 `theme => option` 的函数形式：视图据此使用与页面一致的设计令牌配色。
   */
  option: { type: [Object, Function], default: () => ({}) },
  height: { type: Number, default: 280 },
  loading: { type: Boolean, default: false },
  empty: { type: Boolean, default: false },
  emptyText: { type: String, default: '暂无数据' },
  testId: { type: String, required: true }
})

const container = ref(null)
let chart = null
let observer = null
let unsubscribeMotion = null
const failed = ref(false)

/** 把主题、动画与容器尺寸合并进最终 option。 */
function buildOption() {
  const theme = chartTheme(container.value)
  const own = typeof props.option === 'function' ? props.option(theme) : props.option
  return {
    color: [theme.brandBright, theme.brand, theme.accent, theme.danger, '#8fa3b8'],
    textStyle: { color: theme.text2, fontFamily: 'inherit' },
    ...chartAnimation(),
    ...own
  }
}

function sizeOf() {
  const element = container.value
  if (!element) return { width: 0, height: props.height }
  return { width: element.clientWidth, height: props.height }
}

function render() {
  if (!container.value) return
  if (props.loading || props.empty) return
  const { width, height } = sizeOf()
  // 容器尚未参与布局（例如抽屉内）时等待下一次 resize，避免生成 0×0 画布。
  if (width <= 0 || height <= 0) return
  try {
    if (!chart) {
      chart = initChart(container.value, null, { renderer: 'canvas' })
      if (!chart) {
        failed.value = true
        return
      }
      failed.value = false
    }
    chart.resize({ width, height })
    chart.setOption(buildOption(), true)
  } catch {
    // jsdom 或缺少 canvas 实现时降级为文字提示，绝不能抛出渲染错误。
    failed.value = true
    chart = null
  }
}

/** 等 DOM 完成显示与布局后再初始化，避免 v-show/路由过渡期间拿到 0 宽度。 */
function scheduleRender() {
  nextTick(() => {
    if (typeof requestAnimationFrame === 'function') requestAnimationFrame(render)
    else render()
  })
}

function dispose() {
  if (chart) chart.dispose()
  chart = null
}

onMounted(() => {
  scheduleRender()
  if (typeof ResizeObserver === 'function' && container.value) {
    observer = new ResizeObserver(() => scheduleRender())
    observer.observe(container.value)
  }
  // 系统“减弱动态效果”切换时重新渲染，动画开关随之变化。
  unsubscribeMotion = onReducedMotionChange(() => render())
})

onBeforeUnmount(() => {
  if (observer) observer.disconnect()
  observer = null
  if (unsubscribeMotion) unsubscribeMotion()
  unsubscribeMotion = null
  dispose()
})

watch(
  () => [props.option, props.loading, props.empty],
  () => {
    if (props.loading || props.empty) {
      dispose()
      return
    }
    scheduleRender()
  },
  { deep: true }
)

/** 无障碍：图表只做视觉呈现，数值在相邻表格中可读。 */
const reduced = computed(() => prefersReducedMotion())
</script>

<template>
  <section class="chart-panel panel" :data-testid="testId" :data-reduced-motion="reduced ? 'true' : 'false'">
    <header v-if="title || $slots.actions" class="panel__title">
      <div>
        <h2>{{ title }}</h2>
        <p v-if="hint" class="panel__hint">{{ hint }}</p>
      </div>
      <div class="chart-panel__actions">
        <slot name="actions" />
      </div>
    </header>

    <AppSkeleton v-if="loading" :data-testid="`${testId}-loading`" variant="chart" :rows="1" />

    <p v-else-if="empty" class="empty-state" :data-testid="`${testId}-empty`">{{ emptyText }}</p>

    <p v-else-if="failed" class="empty-state" :data-testid="`${testId}-fallback`">
      当前环境无法绘制图表，请查看下方明细表格。
    </p>

    <div
      v-show="!loading && !empty && !failed"
      ref="container"
      class="chart-panel__canvas"
      :data-testid="`${testId}-canvas`"
      :style="{ height: `${height}px` }"
    ></div>
  </section>
</template>

<style scoped>
.chart-panel {
  display: flex;
  flex-direction: column;
  min-width: 0;
}

.chart-panel__actions {
  display: flex;
  align-items: center;
  gap: var(--ncs-s-2);
}

.chart-panel__canvas {
  width: 100%;
  min-width: 0;
}
</style>
