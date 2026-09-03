<template>
  <div class="tech-card charger-type-card">
    <div class="card-header">
      <span><i class="card-title-decor"></i>快充 / 慢充设备占比</span>
      <span class="card-tag">类型分布</span>
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

  const fastCount = store.summary?.chargerTypeRatio?.fast || 36
  const slowCount = store.summary?.chargerTypeRatio?.slow || 18
  const total = fastCount + slowCount

  const fastPercent = total ? Math.round((fastCount / total) * 100) : 67

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
      right: '8%',
      top: 'center',
      textStyle: { color: '#8da4c4', fontSize: 11 },
      itemGap: 12,
      itemWidth: 10,
      itemHeight: 10
    },
    title: {
      text: `${fastPercent}%`,
      subtext: '快充占比',
      left: '34%',
      top: '38%',
      textAlign: 'center',
      textStyle: {
        fontSize: 18,
        fontWeight: 'bold',
        color: '#00f0ff',
        fontFamily: 'monospace'
      },
      subtextStyle: {
        fontSize: 11,
        color: '#8da4c4'
      }
    },
    series: [
      {
        name: '快慢充比例',
        type: 'pie',
        radius: ['52%', '72%'],
        center: ['35%', '50%'],
        avoidLabelOverlap: false,
        itemStyle: {
          borderRadius: 4,
          borderColor: '#0b111e',
          borderWidth: 2
        },
        label: { show: false },
        labelLine: { show: false },
        data: [
          {
            value: fastCount,
            name: '直流快充 (120kW+)',
            itemStyle: { color: '#00f0ff' }
          },
          {
            value: slowCount,
            name: '交流慢充 (7kW)',
            itemStyle: { color: '#bb86fc' }
          }
        ]
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
.charger-type-card {
  width: 100%;
  height: 100%;
}

.card-tag {
  font-size: 11px;
  color: var(--text-secondary);
}
</style>
