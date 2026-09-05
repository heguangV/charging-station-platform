<template>
  <div class="tech-card station-rank-card">
    <div class="card-header">
      <span><i class="card-title-decor"></i>近30日电站充电量 TOP 5</span>
      <span class="card-tag">单位: kWh</span>
    </div>
    <div class="card-body" ref="chartRef"></div>
    <div v-if="!store.summary?.stationRanking.length" class="chart-empty">{{ store.isLoading ? '加载中…' : store.error && !store.summary ? '数据加载失败' : '暂无数据' }}</div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted, watch } from 'vue'
import * as echarts from '../charts'
import { kwh, escapeHtml } from '../format'
import { useDashboardStore } from '../stores/dashboardStore'
import type { StationRankingItem } from '../types/dashboard'

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

  const ranking: StationRankingItem[] = store.summary?.stationRanking || []

  // 横向柱图从下至上渲染，因此先 reverse
  const sorted = [...ranking].sort((a, b) => b.energyMwh - a.energyMwh).slice(0, 5).reverse()
  const names = sorted.map(item => item.stationName)
  const values = sorted.map(item => kwh(item.energyMwh))

  const option: echarts.EChartsOption = {
    backgroundColor: 'transparent',
    grid: {
      top: '8%',
      left: '3%',
      right: '12%',
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
        const raw = sorted[p.dataIndex]
        return `<b>${escapeHtml(raw.stationName)}</b><br/>
                总充电量: <b>${kwh(raw.energyMwh).toLocaleString()} kWh</b><br/>
                充电次数: ${raw.orderCount} 次<br/>
                累计营收: ¥${(raw.revenueCent / 100).toLocaleString()}`
      }
    },
    xAxis: {
      type: 'value',
      axisLine: { show: false },
      axisTick: { show: false },
      splitLine: {
        lineStyle: {
          color: 'rgba(43, 88, 160, 0.25)',
          type: 'dashed'
        }
      },
      axisLabel: {
        color: '#8da4c4',
        fontSize: 11,
        formatter: (val: number) => (val >= 1000 ? `${(val / 1000).toFixed(0)}k` : `${val}`)
      }
    },
    yAxis: {
      type: 'category',
      data: names,
      axisLine: { lineStyle: { color: 'rgba(43, 88, 160, 0.45)' } },
      axisTick: { show: false },
      axisLabel: {
        color: '#e6f1ff',
        fontSize: 12,
        formatter: (name: string) => (name.length > 7 ? name.slice(0, 7) + '..' : name)
      }
    },
    series: [
      {
        name: '累计充电量',
        type: 'bar',
        data: values,
        barWidth: 14,
        showBackground: true,
        backgroundStyle: {
          color: 'rgba(255, 255, 255, 0.04)',
          borderRadius: 7
        },
        itemStyle: {
          borderRadius: 7,
          color: new echarts.graphic.LinearGradient(0, 0, 1, 0, [
            { offset: 0, color: 'rgba(0, 210, 255, 0.2)' },
            { offset: 1, color: '#00d2ff' }
          ])
        },
        label: {
          show: true,
          position: 'right',
          color: '#00f0ff',
          fontSize: 11,
          fontFamily: 'monospace',
          formatter: (p: any) => p.value >= 1000 ? `${(p.value / 1000).toFixed(1)}k` : `${p.value}`
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
.station-rank-card {
  width: 100%;
  height: 100%;
}

.card-tag {
  font-size: 11px;
  color: var(--text-secondary);
}
</style>
