<template>
  <div class="tech-card charger-status-card">
    <div class="card-header">
      <span><i class="card-title-decor"></i>电桩实时运行状态分布</span>
      <span class="card-tag">总数: {{ store.totalChargers }} 台</span>
    </div>
    <div class="card-body" ref="chartRef"></div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted, watch } from 'vue'
import * as echarts from 'echarts'
import { useDashboardStore } from '../stores/dashboardStore'

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

  const status = store.summary?.chargerStatus || {
    idle: 32,
    inUse: 18,
    fault: 3,
    restarting: 1,
    disabled: 0
  }

  const chartData = [
    { value: status.idle || 0, name: '空闲可用', itemStyle: { color: '#00e676' } },
    { value: status.inUse || 0, name: '充电中', itemStyle: { color: '#ffab00' } },
    { value: status.fault || 0, name: '设备故障', itemStyle: { color: '#ff3d71' } },
    { value: (status.restarting || 0) + (status.disabled || 0), name: '维护/重启', itemStyle: { color: '#00d2ff' } }
  ].filter(item => item.value > 0)

  const option: echarts.EChartsOption = {
    backgroundColor: 'transparent',
    tooltip: {
      trigger: 'item',
      backgroundColor: 'rgba(11, 17, 30, 0.9)',
      borderColor: '#00d2ff',
      textStyle: { color: '#ffffff' },
      formatter: '{b}: <b>{c} 台</b> ({d}%)'
    },
    legend: {
      orient: 'vertical',
      right: '6%',
      top: 'center',
      textStyle: {
        color: '#8da4c4',
        fontSize: 12
      },
      itemGap: 14,
      itemWidth: 10,
      itemHeight: 10
    },
    series: [
      {
        name: '电桩状态',
        type: 'pie',
        radius: ['45%', '72%'],
        center: ['40%', '50%'],
        avoidLabelOverlap: false,
        itemStyle: {
          borderRadius: 6,
          borderColor: '#0b111e',
          borderWidth: 2
        },
        label: {
          show: false
        },
        emphasis: {
          label: {
            show: true,
            fontSize: 13,
            fontWeight: 'bold',
            color: '#ffffff',
            formatter: '{b}\n{d}%'
          },
          scale: true,
          scaleSize: 8
        },
        labelLine: {
          show: false
        },
        data: chartData
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
.charger-status-card {
  width: 100%;
  height: 100%;
}

.card-tag {
  font-size: 12px;
  color: var(--text-secondary);
  font-family: monospace;
}
</style>
