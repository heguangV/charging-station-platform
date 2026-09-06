<template>
  <div class="tech-card load-prediction-card">
    <div class="card-header">
      <span><i class="card-title-decor"></i>未来 24 小时负荷预测 (AI 模型)</span>
      <select v-if="stationIds.length" v-model="selectedStation" aria-label="预测电站"><option v-for="id in stationIds" :key="id" :value="id">电站 {{ id }}</option></select>
    </div>
    <div class="card-body" ref="chartRef"></div>
    <div v-if="!store.summary?.prediction24h.length" class="chart-empty">{{ store.isLoading ? '加载中…' : store.error && !store.summary ? '数据加载失败' : '暂无数据' }}</div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, onMounted, onUnmounted, watch } from 'vue'
import * as echarts from '../charts'
import { kwh, timeLabel, escapeHtml } from '../format'
import { useDashboardStore } from '../stores/dashboardStore'
import type { Prediction24hItem } from '../types/dashboard'

const store = useDashboardStore()
const selectedStation = ref<number | null>(null)
const stationIds = computed(() => [...new Set(store.summary?.prediction24h.map(item => item.stationId) ?? [])])
watch(stationIds, ids => {
  if (!ids.includes(selectedStation.value!)) selectedStation.value = ids[0] ?? null
}, { immediate: true })
const chartRef = ref<HTMLDivElement | null>(null)
let chartInstance: echarts.ECharts | null = null

const initChart = () => {
  if (!chartRef.value) return
  chartInstance = echarts.init(chartRef.value)
  updateChart()
}

const updateChart = () => {
  if (!chartInstance) return

  const list: Prediction24hItem[] = (store.summary?.prediction24h || []).filter(item => item.stationId === selectedStation.value).sort((a, b) => a.targetAt - b.targetAt)
  const hours = list.map(item => timeLabel(item.targetAt))
  const energies = list.map(item => kwh(item.predictedEnergyMwh))
  const freeChargers = list.map(item => item.predictedFreeCount)

  const peakMarks = list
    .filter(item => item.isPeak)
    .map(item => ({
      name: '高峰',
      xAxis: timeLabel(item.targetAt),
      yAxis: kwh(item.predictedEnergyMwh),
      value: '高峰',
      itemStyle: { color: '#c85d50' }
    }))

  const option: echarts.EChartsOption = {
    backgroundColor: 'transparent',
    grid: {
      top: 58,
      left: '8%',
      right: '8%',
      bottom: '8%',
      containLabel: true
    },
    tooltip: {
      trigger: 'axis',
      axisPointer: { type: 'line', lineStyle: { color: 'rgba(46, 149, 98, 0.4)' } },
      backgroundColor: 'rgba(255, 255, 255, 0.98)',
      borderColor: '#287e54',
      textStyle: { color: '#294937' },
      formatter: (params: any) => {
        if (!params || !params.length) return ''
        const idx = params[0].dataIndex
        const raw = list[idx]
        return `${raw.stale ? '预测已过期<br/>' : ''}时间: <b>${escapeHtml(timeLabel(raw.targetAt))}</b><br/>
                预测充电量: <span style="color:#25794e;font-weight:bold">${kwh(raw.predictedEnergyMwh)} kWh</span><br/>
                预测空闲桩: <span style="color:#32945e;font-weight:bold">${raw.predictedFreeCount} 台</span><br/>
                负荷预警: <span style="color:${raw.isPeak ? '#c85d50' : '#32945e'};font-weight:bold">${raw.isPeak ? '⚠️ 用电高峰' : '✅ 平稳区间'}</span>`
      }
    },
    legend: {
      top: 10,
      left: 'center',
      textStyle: { color: '#53616d', fontSize: 12 },
      itemWidth: 10,
      itemHeight: 10
    },
    xAxis: {
      type: 'category',
      data: hours,
      axisLine: { lineStyle: { color: 'rgba(128, 153, 132, 0.45)' } },
      axisTick: { alignWithLabel: true },
      axisLabel: {
        color: '#53616d',
        fontSize: 12,
        interval: 3,
        hideOverlap: true,
        formatter: (value: string) => value.split(' ').pop() || value
      }
    },
    yAxis: [
      {
        type: 'value',
        name: '电量(kWh)',
        nameTextStyle: { color: '#25794e', fontSize: 12, align: 'left' },
        splitLine: {
          lineStyle: {
            color: 'rgba(128, 153, 132, 0.2)',
            type: 'dashed'
          }
        },
        axisLabel: { color: '#53616d', fontSize: 12 }
      },
      {
        type: 'value',
        name: '空闲桩 (台)',
        nameTextStyle: { color: '#96721b', fontSize: 12, align: 'right' },
        splitLine: { show: false },
        axisLabel: { color: '#53616d', fontSize: 12 }
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
        lineStyle: { width: 2.5, color: '#25794e' },
        itemStyle: { color: '#25794e' },
        areaStyle: {
          color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
            { offset: 0, color: 'rgba(46, 149, 98, 0.25)' },
            { offset: 1, color: 'rgba(46, 149, 98, 0.01)' }
          ])
        },
        markPoint: {
          symbol: 'circle',
          symbolSize: 8,
          data: peakMarks,
          label: {
            show: false,
            fontSize: 9,
            color: '#294937'
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
        lineStyle: { width: 2, color: '#b39230', type: 'dashed' },
        itemStyle: { color: '#b39230' }
      }
    ]
  }

  chartInstance.setOption(option, true)
}

watch(
  () => [store.summary, selectedStation.value],
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
.load-prediction-card {
  width: 100%;
  height: 100%;
}

.alert-tag {
  display: flex;
  align-items: center;
  gap: 5px;
  font-size: 11px;
  color: #c85d50;
}

.alert-tag .dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: #c85d50;
  box-shadow: 0 0 6px #c85d50;
  animation: blink 1.5s infinite;
}

@keyframes blink {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.3; }
}
</style>
