<template>
  <div class="tech-card hourly-heatmap-card">
    <div class="card-header">
      <span><i class="card-title-decor"></i>24小时充电负荷时段分布</span>
      <span class="card-tag">日内负荷率</span>
    </div>
    <div class="card-body" ref="chartRef"></div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted, watch } from 'vue'
import * as echarts from 'echarts'
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
  const values = heatmap.map(item => item.loadPercent)

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
        let statusText = '平谷期'
        let color = '#00e676'
        if (val >= 80) {
          statusText = '尖峰高负荷'
          color = '#ff3d71'
        } else if (val >= 60) {
          statusText = '繁忙高峰'
          color = '#ffab00'
        }
        return `时段: <b>${p.name}</b><br/>
                负荷率: <b>${val}%</b><br/>
                状态: <span style="color:${color};font-weight:bold">${statusText}</span>`
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
      max: 100,
      splitLine: {
        lineStyle: {
          color: 'rgba(43, 88, 160, 0.2)',
          type: 'dashed'
        }
      },
      axisLabel: {
        color: '#8da4c4',
        fontSize: 10,
        formatter: '{value}%'
      }
    },
    visualMap: {
      show: false,
      min: 0,
      max: 100,
      inRange: {
        color: ['#00e676', '#00d2ff', '#ffab00', '#ff3d71']
      }
    },
    series: [
      {
        name: '负荷率',
        type: 'bar',
        barWidth: 8,
        data: values,
        itemStyle: {
          borderRadius: [4, 4, 0, 0]
        }
      }
    ]
  }

  chartInstance.setOption(option)
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
