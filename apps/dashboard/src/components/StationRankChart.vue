<template>
  <div class="tech-card station-rank-card">
    <div class="card-header">
      <span><i class="card-title-decor"></i>电站充电量排行 TOP 5</span>
      <span class="card-tag">单位: kWh</span>
    </div>
    <div class="card-body" ref="chartRef"></div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted, watch } from 'vue'
import * as echarts from 'echarts'
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

  const ranking: StationRankingItem[] = store.summary?.stationRanking || [
    { stationId: 1, stationName: '朝阳科技园超级站', chargeCount: 1420, energyKwh: 21300.5, revenueCent: 3195000 },
    { stationId: 2, stationName: '海淀中关村智能站', chargeCount: 1180, energyKwh: 17700.0, revenueCent: 2655000 },
    { stationId: 3, stationName: '丰台金融港快充中心', chargeCount: 890, energyKwh: 13350.2, revenueCent: 2002500 },
    { stationId: 4, stationName: '大兴亦庄能源站', chargeCount: 520, energyKwh: 7800.0, revenueCent: 1170000 },
    { stationId: 5, stationName: '昌平回龙观示范站', chargeCount: 258, energyKwh: 3870.1, revenueCent: 833700 }
  ]

  // 横向柱图从下至上渲染，因此先 reverse
  const sorted = [...ranking].reverse()
  const names = sorted.map(item => item.stationName)
  const values = sorted.map(item => item.energyKwh)

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
        return `<b>${raw.stationName}</b><br/>
                总充电量: <b>${raw.energyKwh.toLocaleString()} kWh</b><br/>
                充电次数: ${raw.chargeCount} 次<br/>
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
          formatter: (p: any) => `${(p.value / 1000).toFixed(1)}k`
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
.station-rank-card {
  width: 100%;
  height: 100%;
}

.card-tag {
  font-size: 11px;
  color: var(--text-secondary);
}
</style>
