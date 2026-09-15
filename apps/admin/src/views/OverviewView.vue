<script setup>
/**
 * 运营总览（接口文档 §8.3、§8.4 与 §6.4）。
 *
 * 内容对齐原 Qt 管理端：营收趋势（近 7/30 日）+ 四个核心指标 + 电桩状态分布与健康度 +
 * 每日营收与订单明细表（点某一行可下钻当天的小时明细）。
 * 所有金额与电量都是服务端返回的整数（分、毫瓦时），只在展示层换算。
 */
import { computed, onMounted } from 'vue'
import AppSkeleton from '@/components/AppSkeleton.vue'
import ChartPanel from '@/components/ChartPanel.vue'
import DataTable from '@/components/DataTable.vue'
import StatCard from '@/components/StatCard.vue'
import StatusPill from '@/components/StatusPill.vue'
import { axisStyle, graphic, tooltipStyle } from '@/charts'
import { chargerStatusBreakdown } from '@/api/stats'
import { useDashboardStore } from '@/stores/dashboard'
import { formatAmount, formatDate, formatDayLabel, formatEnergy, formatHourLabel, formatInt, formatPercent } from '@/utils/format'

const dashboard = useDashboardStore()

onMounted(() => {
  dashboard.loadAll()
})

const hasData = computed(() => dashboard.hasRevenue || dashboard.chargerStatus.totalCount > 0)

/** 每日营收与订单明细：列为日期 / 营收（元）/ 电量（kWh）/ 订单量。 */
const dailyColumns = [
  { key: 'bucketStart', label: '日期', format: value => formatDate(value) },
  { key: 'amountCent', label: '营收（元）', align: 'right', format: value => formatAmount(value) },
  { key: 'energyMwh', label: '电量（kWh）', align: 'right', format: value => formatEnergy(value) },
  { key: 'orderCount', label: '订单量', align: 'right', format: value => formatInt(value) }
]

const hourlyColumns = [
  { key: 'bucketStart', label: '时段', format: value => formatHourLabel(value) },
  { key: 'amountCent', label: '营收（元）', align: 'right', format: value => formatAmount(value) },
  { key: 'energyMwh', label: '电量（kWh）', align: 'right', format: value => formatEnergy(value) },
  { key: 'orderCount', label: '订单量', align: 'right', format: value => formatInt(value) }
]

const breakdown = computed(() => chargerStatusBreakdown(dashboard.chargerStatus))

/** 营收趋势：面积折线（元）+ 订单量柱，坐标轴无轴线、只用发丝虚线网格。 */
const revenueOption = theme => {
  const points = dashboard.points
  const palette = {
    idle: theme.brandBright,
    occupied: '#3f6fb5',
    faulty: theme.danger,
    disabled: '#9aa9ba',
    restarting: theme.accent
  }
  return {
    grid: { left: 4, right: 4, top: 30, bottom: 2, containLabel: true },
    tooltip: { trigger: 'axis', ...tooltipStyle(theme) },
    legend: {
      right: 0,
      top: 0,
      icon: 'roundRect',
      itemWidth: 10,
      itemHeight: 10,
      textStyle: { color: theme.muted, fontSize: 11 },
      data: ['营收（元）', '订单量']
    },
    xAxis: {
      type: 'category',
      data: points.map(point => formatDayLabel(point.bucketStart)),
      boundaryGap: true,
      ...axisStyle(theme),
      splitLine: { show: false }
    },
    yAxis: [
      { type: 'value', name: '元', ...axisStyle(theme) },
      { type: 'value', name: '单', ...axisStyle(theme), splitLine: { show: false } }
    ],
    series: [
      {
        name: '营收（元）',
        type: 'line',
        smooth: true,
        showSymbol: false,
        symbolSize: 6,
        lineStyle: { width: 2.4, color: theme.brand },
        itemStyle: { color: theme.brand },
        areaStyle: {
          color: new graphic.LinearGradient(0, 0, 0, 1, [
            { offset: 0, color: 'rgba(20, 161, 146, 0.28)' },
            { offset: 1, color: 'rgba(20, 161, 146, 0)' }
          ])
        },
        data: points.map(point => Number(point.amount.toFixed(2)))
      },
      {
        name: '订单量',
        type: 'bar',
        yAxisIndex: 1,
        barWidth: '36%',
        itemStyle: { borderRadius: [4, 4, 0, 0], color: 'rgba(224, 138, 30, 0.55)' },
        data: points.map(point => point.orderCount)
      }
    ]
  }
}

/** 设备状态环形图：只显示数量非零的分段，中心留白由健康度文字占据。 */
const statusOption = theme => {
  const palette = {
    idle: theme.brandBright,
    occupied: '#3f6fb5',
    faulty: theme.danger,
    disabled: '#9aa9ba',
    restarting: theme.accent
  }
  return {
    tooltip: { trigger: 'item', ...tooltipStyle(theme) },
    series: [
      {
        type: 'pie',
        radius: ['60%', '84%'],
        center: ['50%', '52%'],
        avoidLabelOverlap: true,
        label: { show: false },
        labelLine: { show: false },
        itemStyle: { borderColor: theme.surface, borderWidth: 2 },
        data: breakdown.value
          .filter(item => item.value > 0)
          .map(item => ({ name: item.label, value: item.value, itemStyle: { color: palette[item.key] } }))
      }
    ]
  }
}

const revenueEmpty = computed(() => !dashboard.loading && dashboard.points.length === 0)
const statusEmpty = computed(() => !dashboard.loading && dashboard.chargerStatus.totalCount === 0)

function selectDay(row) {
  dashboard.selectDay(row.bucketStart)
}
</script>

<template>
  <div class="view overview-view">
    <p v-if="dashboard.error" class="alert alert--error" data-testid="overview-error">
      {{ dashboard.error }}
      <span class="alert__actions">
        <button type="button" class="btn btn--sm" data-testid="overview-retry" @click="dashboard.loadAll()">重试</button>
      </span>
    </p>

    <AppSkeleton v-if="dashboard.loading && !hasData" data-testid="overview-loading" variant="kpi" :rows="4" />

    <section class="overview-kpis" v-reveal>
      <StatCard
        label="今日营收（元）"
        test-id="overview-kpi-revenue-today"
        format="amount"
        :value="dashboard.today.amountCent"
        :loading="dashboard.loading && !hasData"
        :hint="`今日订单 ${formatInt(dashboard.today.orderCount)} 单（仅统计已结算订单）`"
      />
      <StatCard
        label="本月营收（元）"
        test-id="overview-kpi-revenue-month"
        format="amount"
        :value="dashboard.month.amountCent"
        :loading="dashboard.loading && !hasData"
        :hint="`本月电量 ${formatEnergy(dashboard.month.energyMwh)} kWh`"
      />
      <StatCard
        label="可运营电桩"
        test-id="overview-kpi-operational"
        format="count"
        :value="dashboard.operationalCount"
        :loading="dashboard.loading && !hasData"
        :hint="`总设备 ${formatInt(dashboard.totalChargers)} 台`"
      />
      <StatCard
        label="注册用户"
        test-id="overview-kpi-users"
        format="count"
        :value="dashboard.userTotal"
        :loading="dashboard.loading && !hasData"
        hint="平台注册用户总数"
      />
      <StatCard
        label="设备健康度"
        test-id="overview-kpi-health"
        format="percent"
        tone="accent"
        :value="dashboard.healthPercent"
        :loading="dashboard.loading && !hasData"
        hint="可运营/总设备"
      />
    </section>

    <section class="overview-charts">
      <ChartPanel
        test-id="overview-revenue-chart"
        title="营收趋势"
        :hint="`最近 ${dashboard.rangeDays} 天 · 金额单位为元`"
        :option="revenueOption"
        :loading="dashboard.loading && !hasData"
        :empty="revenueEmpty"
        empty-text="所选区间暂无已结算订单"
        :height="300"
      >
        <template #actions>
          <button
            type="button"
            class="chip"
            :class="{ 'is-active': dashboard.rangeDays === 7 }"
            data-testid="overview-range-7"
            @click="dashboard.setRangeDays(7)"
          >
            近 7 日
          </button>
          <button
            type="button"
            class="chip"
            :class="{ 'is-active': dashboard.rangeDays === 30 }"
            data-testid="overview-range-30"
            @click="dashboard.setRangeDays(30)"
          >
            近 30 日
          </button>
        </template>
      </ChartPanel>

      <ChartPanel
        test-id="overview-charger-chart"
        title="电桩状态"
        hint="含停用与重启中的全部设备"
        :option="statusOption"
        :loading="dashboard.loading && !hasData"
        :empty="statusEmpty"
        empty-text="暂无设备数据"
        :height="220"
      >
        <template #actions>
          <span class="muted" data-testid="overview-health-label">
            健康度 {{ formatPercent(dashboard.healthPercent) }}
          </span>
        </template>
      </ChartPanel>
    </section>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>状态明细</h2>
          <p class="panel__hint">空闲 / 在用 / 故障 / 已停用 / 重启中 的真实数量</p>
        </div>
      </div>
      <ul class="metric-list" data-testid="overview-status-list">
        <li v-for="item in breakdown" :key="item.key" :data-testid="`overview-status-${item.key}`">
          <StatusPill :text="item.label" :tone="item.tone" />
          <strong>{{ formatInt(item.value) }} 台</strong>
        </li>
      </ul>
    </section>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>每日营收与订单</h2>
          <p class="panel__hint">点击某一天可查看当天的小时明细（仅统计已完成并结算成功的订单）</p>
        </div>
        <button type="button" class="btn btn--sm" data-testid="overview-refresh" @click="dashboard.loadAll()">
          刷新
        </button>
      </div>

      <DataTable
        test-id="overview-daily"
        :columns="dailyColumns"
        :rows="dashboard.revenue.items"
        row-key="bucketStart"
        :loading="dashboard.loading && !hasData"
        empty-text="所选区间暂无营收记录"
        :selected-key="dashboard.selectedBucketStart"
        :row-test-id="row => `overview-day-${row.bucketStart}`"
        @row-click="selectDay"
        @retry="dashboard.loadAll()"
      />

      <p v-if="dashboard.selectedPoint" class="overview-summary" data-testid="overview-day-summary">
        {{ formatDate(dashboard.selectedPoint.bucketStart) }}：营收
        {{ formatAmount(dashboard.selectedPoint.amountCent) }} 元 · 电量
        {{ formatEnergy(dashboard.selectedPoint.energyMwh) }} kWh · 订单
        {{ formatInt(dashboard.selectedPoint.orderCount) }} 单
      </p>

      <h3 class="overview-detail-title">当天小时明细</h3>
      <p v-if="dashboard.dayDetailError" class="alert alert--error" data-testid="overview-day-detail-error">
        {{ dashboard.dayDetailError }}
      </p>
      <AppSkeleton
        v-else-if="dashboard.dayDetailLoading"
        data-testid="overview-day-detail-loading"
        variant="table"
        :rows="3"
      />
      <DataTable
        v-else
        test-id="overview-day-detail"
        :columns="hourlyColumns"
        :rows="dashboard.hourlyDetail"
        row-key="bucketStart"
        empty-text="请选择上表某一天查看小时明细"
      />
    </section>

    <p v-if="!dashboard.loading && !dashboard.error && !hasData" class="empty-state" data-testid="overview-empty">
      暂无运营数据：当前区间内没有已结算订单，也没有设备记录。
    </p>
  </div>
</template>

<style scoped>
.overview-kpis {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(190px, 1fr));
  gap: var(--ncs-s-3);
}

.overview-charts {
  display: grid;
  grid-template-columns: minmax(0, 1.7fr) minmax(0, 1fr);
  gap: var(--ncs-s-4);
  align-items: start;
}

.overview-summary {
  margin: var(--ncs-s-3) 0 0;
  font-size: var(--ncs-fs-sm);
  color: var(--ncs-text-2);
  font-variant-numeric: tabular-nums;
}

.overview-detail-title {
  margin: var(--ncs-s-4) 0 var(--ncs-s-2);
  font-size: var(--ncs-fs-sm);
  color: var(--ncs-muted);
  font-weight: 600;
}

@media (max-width: 1023px) {
  .overview-charts {
    grid-template-columns: minmax(0, 1fr);
  }
}
</style>
