<template>
  <div class="tech-card load-prediction-card">
    <div class="card-header">
      <span><i class="card-title-decor"></i>未来 24 小时负荷预测 (AI 模型)</span>
      <span class="card-tag alert-tag"><i class="dot"></i>含高峰预警</span>
    </div>
    <div class="card-body" ref="chartRef"></div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted, watch } from 'vue'
import * as echarts from 'echarts'
import { useDashboardStore } from '../stores/dashboardStore'
import type { Prediction24hItem } from '../types/dashboard'

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

  const list: Prediction24hItem[] = store.summary?.prediction24h || []
  const hours = list.map(item => item.hourLabel)
  const energies = list.map(item => item.predictedEnergy)
  const freeChargers = list.map(item => item.predictedFreeChargers)

  const peakMarks = list
    .filter(item => item.isPeak === 1)
    .map(item => ({
      name: '高峰',
      xAxis: item.hourLabel,
      yAxis: item.predictedEnergy,
      value: '高峰',
      itemStyle: { color: '#ff3d71' }
    }))

  const option: echarts.EChartsOption = {
    backgroundColor: 'transparent',
    grid: {
      top: '18%',
      left: '3%',
      right: '8%',
      bottom: '8%',
      containLabel: true
    },
    tooltip: {
      trigger: 'axis',
      axisPointer: { type: 'line', lineStyle: { color: 'rgba(0, 210, 255, 0.4)' } },
      backgroundColor: 'rgba(11, 17, 30, 0.92)',
      borderColor: '#00d2ff',
      textStyle: { color: '#ffffff' },
      formatter: (params: any) => {
        if (!params || !params.length) return ''
        const idx = params[0].dataIndex
        const raw = list[idx]
        return `时间: <b>${raw.hourLabel}</b><br/>
                预测充电量: <span style="color:#00f0ff;font-weight:bold">${raw.predictedEnergy} kWh</span><br/>
                预测空闲桩: <span style="color:#00e676;font-weight:bold">${raw.predictedFreeChargers} 台</span><br/>
                负荷预警: <span style="color:${raw.isPeak ? '#ff3d71' : '#00e676'};font-weight:bold">${raw.isPeak ? '⚠️ 用电高峰' : '✅ 平稳区间'}</span>`
      }
    },
    legend: {
      top: '2%',
      right: '4%',
      textStyle: { color: '#8da4c4', fontSize: 11 },
      itemWidth: 10,
      itemHeight: 10
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
    yAxis: [
      {
        type: 'value',
        name: '预测负荷 (kWh)',
        nameTextStyle: { color: '#00f0ff', fontSize: 10 },
        splitLine: {
          lineStyle: {
            color: 'rgba(43, 88, 160, 0.2)',
            type: 'dashed'
          }
        },
        axisLabel: { color: '#8da4c4', fontSize: 10 }
      },
      {
        type: 'value',
        name: '空闲桩 (台)',
        nameTextStyle: { color: '#00e676', fontSize: 10 },
        splitLine: { show: false },
        axisLabel: { color: '#8da4c4', fontSize: 10 }
      }
    ],
    series: [
      {
        name: '预测负荷',
        type: 'line',
        smooth: true,
        data: energies,
        yAxisIndex: 0,
        showSymbol: false,
        lineStyle: { width: 2.5, color: '#00f0ff' },
        itemStyle: { color: '#00f0ff' },
        areaStyle: {
          color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
            { offset: 0, color: 'rgba(0, 240, 255, 0.25)' },
            { offset: 1, color: 'rgba(0, 240, 255, 0.01)' }
          ])
        },
        markPoint: {
          symbol: 'pin',
          symbolSize: 32,
          data: peakMarks,
          label: {
            fontSize: 9,
            color: '#ffffff'
          }
        }
      },
      {
        name: '预测空闲桩',
        type: 'line',
        smooth: true,
        data: freeChargers,
        yAxisIndex: 1,
        showSymbol: false,
        lineStyle: { width: 2, color: '#00e676', type: 'dashed' },
        itemStyle: { color: '#00e676' }
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
.load-prediction-card {
  width: 100%;
  height: 100%;
}

.alert-tag {
  display: flex;
  align-items: center;
  gap: 5px;
  font-size: 11px;
  color: #ff3d71;
}

.alert-tag .dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: #ff3d71;
  box-shadow: 0 0 6px #ff3d71;
  animation: blink 1.5s infinite;
}

@keyframes blink {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.3; }
}
</style>
