<template>
  <div class="tech-card hourly-heatmap-card">
    <div class="card-header">
      <span><i class="card-title-decor"></i>近30日充电时段分布</span>
      <span class="card-tag">UTC 时段 · kWh</span>
    </div>
    <div class="card-body" ref="chartRef"></div>
    <div v-if="!store.summary?.hourlyHeatmap.some(item => item.energyMwh > 0)" class="chart-empty">{{ store.isLoading ? '加载中…' : store.error && !store.summary ? '数据加载失败' : '暂无数据' }}</div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted, watch } from 'vue'
import * as echarts from '../charts'
import { kwh } from '../format'
import { useDashboardStore } from '../stores/dashboardStore'
import type { HourlyHeatmapItem } from '../types/dashboard'

const store = useDashboardStore()
const chartRef = ref<HTMLDivElement | null>(null)
let chartInstance: echarts.ECharts | null = null

const initChart = () => {
  if (!chartRef.value) return
  chartInstance = echarts.init(chartRef.value)
  updateChart()
}

const updateChart = () => {
  if (!chartInstance) return

  const heatmap: HourlyHeatmapItem[] = store.summary?.hourlyHeatmap || []
  const hours = Array.from({ length: 24 }, (_, i) => `${i}:00`)
  const values = Array.from({ length: 24 }, (_, hour) => kwh(heatmap.filter(item => item.hour === hour).reduce((sum, item) => sum + item.energyMwh, 0)))

  const option: echarts.EChartsOption = {
    backgroundColor: 'transparent',
    grid: {
      top: '12%',
      left: '3%',
      right: '4%',
      bottom: '6%',
      containLabel: true
    },
    tooltip: {
      trigger: 'axis',
      axisPointer: { type: 'shadow' },
      backgroundColor: 'rgba(11, 17, 30, 0.9)',
      borderColor: '#00d2ff',
      textStyle: { color: '#ffffff' },
      formatter: (params: any) => {
        const p = params[0]
        const val = p.value
        return `时段: ${p.dataIndex}:00 UTC<br/>充电量: ${Number(val).toLocaleString()} kWh`
      }
    },
    xAxis: {
      type: 'category',
      data: hours,
      axisLine: { lineStyle: { color: 'rgba(43, 88, 160, 0.45)' } },
      axisTick: { alignWithLabel: true },
      axisLabel: {
        color: '#8da4c4',
        fontSize: 10,
        interval: 3
      }
    },
    yAxis: {
      type: 'value',
      splitLine: {
        lineStyle: {
          color: 'rgba(43, 88, 160, 0.2)',
          type: 'dashed'
        }
      },
      axisLabel: {
        color: '#8da4c4',
        fontSize: 10,
        formatter: '{value}'
      }
    },
    visualMap: {
      show: false,
      min: 0,
      max: Math.max(1, ...values),
      inRange: {
        color: ['#00e676', '#00d2ff', '#ffab00', '#ff3d71']
      }
    },
    series: [
      {
        name: '充电量',
        type: 'bar',
        barWidth: 8,
        data: values,
        itemStyle: {
          borderRadius: [4, 4, 0, 0]
        }
      }
    ]
  }

  chartInstance.setOption(option, true)
}

watch(
  () => store.summary,
  () => {
    updateChart()
  },
  { deep: true }
)

const handleResize = () => {
  chartInstance?.resize()
}

onMounted(() => {
  initChart()
  window.addEventListener('resize', handleResize)
})

onUnmounted(() => {
  window.removeEventListener('resize', handleResize)
  chartInstance?.dispose()
})
</script>

<style scoped>
.hourly-heatmap-card {
  width: 100%;
  height: 100%;
}

.card-tag {
  font-size: 11px;
  color: var(--text-secondary);
}
</style>
