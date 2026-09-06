<template>
  <div class="tech-card revenue-trend-card">
    <div class="card-header">
      <span><i class="card-title-decor"></i>近 30 日运营趋势（营收与充电量）</span>
      <div class="header-legend">
        <span class="legend-badge revenue"><i class="dot"></i>营收 (元)</span>
        <span class="legend-badge energy"><i class="dot"></i>电量 (kWh)</span>
      </div>
    </div>
    <div class="card-body" ref="chartRef"></div>
    <div v-if="!store.summary?.revenue30d.length" class="chart-empty">{{ store.isLoading ? '加载中…' : store.error && !store.summary ? '数据加载失败' : '暂无数据' }}</div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted, watch } from 'vue'
import * as echarts from '../charts'
import { kwh, escapeHtml, dateLabel } from '../format'
import { useDashboardStore } from '../stores/dashboardStore'
import type { DailyRevenueItem } from '../types/dashboard'

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

  const list: DailyRevenueItem[] = store.summary?.revenue30d || []
  const dates = list.map(item => dateLabel(item.bucketAt))
  const revenues = list.map(item => Math.round((item.revenueCent / 100) * 100) / 100)
  const energies = list.map(item => kwh(item.energyMwh))

  const option: echarts.EChartsOption = {
    backgroundColor: 'transparent',
    grid: {
      top: '16%',
      left: '4%',
      right: '4%',
      bottom: '8%',
      containLabel: true
    },
    tooltip: {
      trigger: 'axis',
      axisPointer: {
        type: 'cross',
        crossStyle: { color: 'rgba(46, 149, 98, 0.4)' }
      },
      backgroundColor: 'rgba(255, 255, 255, 0.98)',
      borderColor: '#287e54',
      textStyle: { color: '#294937' },
      formatter: (params: any) => {
        if (!params || !params.length) return ''
        const idx = params[0].dataIndex
        const raw = list[idx]
        return `<b>${escapeHtml(dateLabel(raw.bucketAt))}</b><br/>
                当日营收: <span style="color:#96721b;font-weight:bold">¥${(raw.revenueCent / 100).toFixed(2)}</span><br/>
                充电总量: <span style="color:#287e54;font-weight:bold">${kwh(raw.energyMwh).toLocaleString()} kWh</span><br/>
                充电车次: ${raw.orderCount} 次`
      }
    },
    xAxis: {
      type: 'category',
      data: dates,
      axisLine: { lineStyle: { color: 'rgba(128, 153, 132, 0.45)' } },
      axisTick: { alignWithLabel: true },
      axisLabel: {
        color: '#53616d',
        fontSize: 12,
        interval: 3,
        hideOverlap: true
      }
    },
    yAxis: [
      {
        type: 'value',
        name: '营收 (元)',
        nameTextStyle: { color: '#96721b', fontSize: 12, align: 'left' },
        splitLine: {
          lineStyle: {
            color: 'rgba(128, 153, 132, 0.2)',
            type: 'dashed'
          }
        },
        axisLabel: {
          color: '#53616d',
          fontSize: 12,
          formatter: (val: number) => val >= 1000 ? `¥${(val / 1000).toFixed(1)}k` : `¥${val}`
        }
      },
      {
        type: 'value',
        name: '电量 (kWh)',
        nameTextStyle: { color: '#287e54', fontSize: 12, align: 'right' },
        splitLine: { show: false },
        axisLabel: {
          color: '#53616d',
          fontSize: 12,
          formatter: (val: number) => val >= 1000 ? `${(val / 1000).toFixed(1)}k` : `${val}`
        }
      }
    ],
    series: [
      {
        name: '营收 (元)',
        type: 'line',
        smooth: true,
        data: revenues,
        yAxisIndex: 0,
        showSymbol: false,
        symbolSize: 6,
        lineStyle: {
          width: 3,
          color: '#d2a52e'
        },
        itemStyle: { color: '#d2a52e' },
        areaStyle: {
          color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
            { offset: 0, color: 'rgba(210, 165, 46, 0.35)' },
            { offset: 1, color: 'rgba(210, 165, 46, 0.01)' }
          ])
        }
      },
      {
        name: '电量 (kWh)',
        type: 'line',
        smooth: true,
        data: energies,
        yAxisIndex: 1,
        showSymbol: false,
        symbolSize: 6,
        lineStyle: {
          width: 2.5,
          color: '#287e54',
          type: 'solid'
        },
        itemStyle: { color: '#287e54' },
        areaStyle: {
          color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
            { offset: 0, color: 'rgba(46, 149, 98, 0.25)' },
            { offset: 1, color: 'rgba(46, 149, 98, 0.01)' }
          ])
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

const resizeObserver = new ResizeObserver(handleResize)

onMounted(() => {
  initChart()
  if (chartRef.value) resizeObserver.observe(chartRef.value)
})

onUnmounted(() => {
  resizeObserver.disconnect()
  chartInstance?.dispose()
})
</script>

<style scoped>
.revenue-trend-card {
  width: 100%;
  height: 100%;
}

.header-legend {
  display: flex;
  gap: 14px;
  font-size: 11px;
}

.legend-badge {
  display: flex;
  align-items: center;
  gap: 5px;
  color: var(--text-secondary);
}

.legend-badge.revenue .dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #d2a52e;
  box-shadow: 0 0 6px #d2a52e;
}

.legend-badge.energy .dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #287e54;
  box-shadow: 0 0 6px #287e54;
}
</style>
