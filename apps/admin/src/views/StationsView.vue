<script setup>
/**
 * 站点管理（接口文档 §7.1–§7.5、§7.11–§7.13）。
 *
 * 对齐 Qt 管理端的能力并补齐价格部分：
 * 站点检索与启停、组合创建（站点 + 初始设备）、编辑（乐观锁 version）、
 * 基础价格版本列表与创建、服务费调整审批。
 * 所有写入都会先正常发起，服务端要求重新验证时由 auth store 弹窗后用同一幂等键重试。
 */
import { computed, onMounted, ref } from 'vue'
import ConfirmDialog from '@/components/ConfirmDialog.vue'
import DataTable from '@/components/DataTable.vue'
import FilterBar from '@/components/FilterBar.vue'
import FormDialog from '@/components/FormDialog.vue'
import StatusPill from '@/components/StatusPill.vue'
import { useAuthStore } from '@/stores/auth'
import { useStationsStore } from '@/stores/stations'
import { CHARGER_TYPES, STATION_STATUS, ADJUSTMENT_SOURCES } from '@/utils/domain'
import { formatAmount, formatDateTime, fromDateTimeInputValue } from '@/utils/format'

const stations = useStationsStore()
const auth = useAuthStore()

const keyword = ref('')
const statusFilter = ref(null)
const adcodeFilter = ref('')

const createOpen = ref(false)
const editTarget = ref(null)
const toggleTarget = ref(null)
const tariffOpen = ref(false)
const adjustmentOpen = ref(false)

onMounted(async () => {
  await stations.load()
  await stations.loadTariffs()
})

const stationColumns = [
  { key: 'code', label: '站点编码' },
  { key: 'name', label: '站点名称' },
  { key: 'adcode', label: '行政区编码' },
  {
    key: 'enabled',
    label: '运营状态',
    format: value => (value === true ? '运营中' : '已停用')
  },
  { key: 'version', label: '版本', align: 'right' }
]

const tariffColumns = [
  { key: 'adcode', label: '行政区编码' },
  {
    key: 'electricityPriceCentPerKwh',
    label: '电费（元/kWh）',
    align: 'right',
    format: value => formatAmount(value)
  },
  {
    key: 'servicePriceCentPerKwh',
    label: '服务费（元/kWh）',
    align: 'right',
    format: value => formatAmount(value)
  },
  { key: 'effectiveFrom', label: '生效时间', format: value => formatDateTime(value) },
  { key: 'effectiveTo', label: '失效时间', format: value => (value ? formatDateTime(value) : '长期有效') }
]

/** 新增站点：经纬度按十进制度输入，提交前换算为整数 E6 度。 */
const createFields = [
  { key: 'code', label: '站点编码', required: true, minLength: 2, maxLength: 16, placeholder: '如 ZGC2' },
  { key: 'name', label: '站点名称', required: true, maxLength: 64 },
  { key: 'address', label: '地址', required: true, maxLength: 128 },
  { key: 'adcode', label: '行政区编码', required: true, minLength: 6, maxLength: 6, placeholder: '6 位行政区编码' },
  { key: 'latitude', label: '纬度（度）', type: 'number', integer: false, required: true, min: -90, max: 90 },
  { key: 'longitude', label: '经度（度）', type: 'number', integer: false, required: true, min: -180, max: 180 },
  { key: 'businessHours', label: '营业时间', default: '00:00-24:00', maxLength: 64 },
  { key: 'count', label: '初始电桩数量', type: 'number', required: true, min: 1, max: 100, default: 4 },
  {
    key: 'chargerType',
    label: '电桩类型',
    type: 'select',
    options: CHARGER_TYPES.map(item => ({ value: item.value, label: item.label }))
  },
  { key: 'powerWatt', label: '单桩功率（W）', type: 'number', required: true, min: 1, max: 1000000, default: 60000 },
  { key: 'connectorStandard', label: '接口标准', default: 'GB/T 20234.3', maxLength: 32 }
]

const editFields = computed(() => [
  { key: 'name', label: '站点名称', required: true, maxLength: 64 },
  { key: 'address', label: '地址', required: true, maxLength: 128 },
  { key: 'adcode', label: '行政区编码', required: true, minLength: 6, maxLength: 6 },
  { key: 'latitude', label: '纬度（度）', type: 'number', integer: false, required: true, min: -90, max: 90 },
  { key: 'longitude', label: '经度（度）', type: 'number', integer: false, required: true, min: -180, max: 180 },
  { key: 'businessHours', label: '营业时间', maxLength: 64 }
])

const tariffFields = [
  { key: 'adcode', label: '行政区编码', required: true, minLength: 6, maxLength: 6 },
  { key: 'electricityPriceCentPerKwh', label: '电费（分/kWh）', type: 'number', required: true, min: 0, max: 100000 },
  { key: 'servicePriceCentPerKwh', label: '服务费（分/kWh）', type: 'number', required: true, min: 0, max: 100000 },
  { key: 'effectiveFrom', label: '生效时间', type: 'datetime', required: true },
  { key: 'effectiveTo', label: '失效时间（可空）', type: 'datetime' },
  { key: 'reason', label: '调整原因', required: true, minLength: 2, maxLength: 200 }
]

const adjustmentFields = [
  { key: 'stationId', label: '站点 ID', type: 'number', required: true, min: 1 },
  {
    key: 'chargerType',
    label: '电桩类型',
    type: 'select',
    options: CHARGER_TYPES.map(item => ({ value: item.value, label: item.label }))
  },
  {
    key: 'source',
    label: '调整来源',
    type: 'select',
    options: ADJUSTMENT_SOURCES.map(item => ({ value: item.value, label: item.label }))
  },
  {
    key: 'adjustmentBp',
    label: '调整幅度（bp）',
    type: 'number',
    required: true,
    min: -2000,
    max: 2000,
    help: '-2000~2000，步长 500，仅调整服务费'
  },
  { key: 'effectiveFrom', label: '生效时间', type: 'datetime', required: true },
  { key: 'effectiveTo', label: '失效时间', type: 'datetime', required: true },
  { key: 'reason', label: '调整原因', required: true, minLength: 2, maxLength: 200 }
]

const toggleHint = computed(() => {
  if (!toggleTarget.value) return ''
  const target = toggleTarget.value.enabled ? '停用' : '启用'
  return `确认${target}站点「${toggleTarget.value.name}」？存在活动流程的站点无法停用，停用不删除历史数据。`
})

function toE6(degrees) {
  return Math.round(Number(degrees) * 1000000)
}

async function submitCreate(values) {
  const ok = await stations.create({
    code: values.code,
    name: values.name,
    address: values.address,
    adcode: values.adcode,
    latitudeE6: toE6(values.latitude),
    longitudeE6: toE6(values.longitude),
    businessHours: values.businessHours || '00:00-24:00',
    initialCharger: {
      count: values.count,
      chargerType: values.chargerType,
      powerWatt: values.powerWatt,
      connectorStandard: values.connectorStandard || 'GB/T 20234.3'
    }
  })
  if (ok) createOpen.value = false
}

async function submitEdit(values) {
  if (!editTarget.value) return
  const ok = await stations.edit(editTarget.value, {
    name: values.name,
    address: values.address,
    adcode: values.adcode,
    latitudeE6: toE6(values.latitude),
    longitudeE6: toE6(values.longitude),
    businessHours: values.businessHours || ''
  })
  if (ok) editTarget.value = null
}

async function submitToggle({ reason }) {
  if (!toggleTarget.value) return
  const ok = await stations.setEnabled(toggleTarget.value, !toggleTarget.value.enabled, reason)
  if (ok) toggleTarget.value = null
}

async function submitTariff(values) {
  const ok = await stations.createTariffVersion({
    adcode: values.adcode,
    electricityPriceCentPerKwh: values.electricityPriceCentPerKwh,
    servicePriceCentPerKwh: values.servicePriceCentPerKwh,
    effectiveFrom: fromDateTimeInputValue(values.effectiveFrom),
    effectiveTo: values.effectiveTo ? fromDateTimeInputValue(values.effectiveTo) : null,
    reason: values.reason
  })
  if (ok) tariffOpen.value = false
}

async function submitAdjustment(values) {
  const ok = await stations.approveAdjustment({
    stationId: values.stationId,
    chargerType: values.chargerType,
    source: values.source,
    adjustmentBp: values.adjustmentBp,
    effectiveFrom: fromDateTimeInputValue(values.effectiveFrom),
    effectiveTo: fromDateTimeInputValue(values.effectiveTo),
    reason: values.reason
  })
  if (ok) adjustmentOpen.value = false
}

function applyFilters() {
  stations.setFilter({ keyword: keyword.value, status: statusFilter.value, adcode: adcodeFilter.value })
}

function resetFilters() {
  keyword.value = ''
  statusFilter.value = null
  adcodeFilter.value = ''
  stations.resetFilters()
}

function statusTone(enabled) {
  return enabled === true ? 'ok' : 'muted'
}
</script>

<template>
  <div class="view">
    <p v-if="stations.notice" class="alert alert--ok" data-testid="stations-notice">
      {{ stations.notice }}
    </p>
    <p v-if="stations.error" class="alert alert--error" data-testid="stations-error-banner">
      {{ stations.error }}
    </p>
    <p v-if="stations.conflict" class="alert" data-testid="stations-conflict">
      {{ stations.conflict }}
      <span class="alert__actions">
        <button type="button" class="btn btn--sm" @click="stations.load()">刷新列表</button>
      </span>
    </p>
    <p v-if="!auth.isOwner" class="alert" data-testid="stations-permission-hint">
      当前账号非 OWNER：站点启停、价格版本与价格调整需要 OWNER 权限，服务端会拒绝越权请求。
    </p>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>站点检索</h2>
          <p class="panel__hint">支持名称/地址关键词、行政区编码（6 位）与运营状态过滤</p>
        </div>
        <div class="panel__row">
          <button type="button" class="btn btn--sm btn--primary" data-testid="station-create" @click="createOpen = true">
            新增站点
          </button>
        </div>
      </div>

      <FilterBar test-id="stations-filter" :busy="stations.loading" @submit="applyFilters">
        <label class="field">
          <span>关键词</span>
          <input v-model="keyword" type="search" placeholder="站点名称或地址" data-testid="stations-keyword" />
        </label>
        <label class="field">
          <span>行政区编码</span>
          <input v-model="adcodeFilter" type="text" placeholder="6 位编码" data-testid="stations-adcode" />
        </label>
        <label class="field">
          <span>运营状态</span>
          <select v-model="statusFilter" data-testid="stations-status">
            <option :value="null">全部状态</option>
            <option v-for="item in STATION_STATUS" :key="item.value" :value="item.value">{{ item.label }}</option>
          </select>
        </label>
        <template #actions>
          <button type="button" class="btn" data-testid="stations-reset" @click="resetFilters">重置</button>
        </template>
      </FilterBar>
    </section>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>站点列表</h2>
          <p class="panel__hint">
            共 {{ stations.total }} 个站点 · 当前页运营中 {{ stations.enabledCount }} 个
          </p>
        </div>
      </div>

      <DataTable
        test-id="stations"
        :columns="stationColumns"
        :rows="stations.items"
        row-key="id"
        :loading="stations.loading"
        :error="stations.error"
        empty-text="没有匹配的站点，可调整检索条件或新增站点"
        :selected-key="stations.selectedStationId"
        :row-test-id="row => `station-row-${row.id}`"
        @row-click="row => stations.select(row.id)"
        @retry="stations.load()"
      >
        <template #cell-enabled="{ row }">
          <StatusPill :text="row.enabled ? '运营中' : '已停用'" :tone="statusTone(row.enabled)" :test-id="`station-state-${row.id}`" />
        </template>
        <template #cell-version="{ row }">
          <span class="row-actions">
            <button
              type="button"
              class="btn btn--sm"
              :data-testid="`station-edit-${row.id}`"
              @click.stop="editTarget = row"
            >
              编辑
            </button>
            <button
              type="button"
              class="btn btn--sm"
              :class="row.enabled ? 'btn--danger' : 'btn--ghost'"
              :data-testid="`station-toggle-${row.id}`"
              @click.stop="toggleTarget = row"
            >
              {{ row.enabled ? '停用' : '启用' }}
            </button>
          </span>
        </template>
      </DataTable>

      <div class="panel__row panel__row--end pager" data-testid="stations-pager">
        <button
          type="button"
          class="btn btn--sm"
          :disabled="stations.page <= 1"
          data-testid="stations-prev"
          @click="stations.goToPage(stations.page - 1)"
        >
          上一页
        </button>
        <span class="muted">第 {{ stations.page }} / {{ stations.pageCount }} 页</span>
        <button
          type="button"
          class="btn btn--sm"
          :disabled="stations.page >= stations.pageCount"
          data-testid="stations-next"
          @click="stations.goToPage(stations.page + 1)"
        >
          下一页
        </button>
      </div>
    </section>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>基础价格版本</h2>
          <p class="panel__hint">同一行政区有效期不得重叠；已生成的订单价格快照不受影响</p>
        </div>
        <div class="panel__row">
          <button type="button" class="btn btn--sm" data-testid="tariff-create" @click="tariffOpen = true">
            新建价格版本
          </button>
          <button type="button" class="btn btn--sm btn--ghost" data-testid="adjustment-create" @click="adjustmentOpen = true">
            服务费调整
          </button>
        </div>
      </div>

      <p v-if="stations.tariffsError" class="alert alert--error" data-testid="tariffs-error">{{ stations.tariffsError }}</p>

      <DataTable
        test-id="tariffs"
        :columns="tariffColumns"
        :rows="stations.tariffs"
        row-key="adcode"
        :loading="stations.tariffsLoading"
        :error="stations.tariffsError"
        empty-text="暂无价格版本，可按行政区创建"
      />
    </section>

    <FormDialog
      v-if="createOpen"
      test-id="station-create-dialog"
      title="新增站点"
      hint="站点与初始设备在同一事务内创建；行政区必须已存在生效的基础价格版本"
      :fields="createFields"
      submit-label="创建站点"
      :loading="stations.saving"
      :error="stations.error"
      @submit="submitCreate"
      @cancel="createOpen = false"
    />

    <FormDialog
      v-if="editTarget"
      test-id="station-edit-dialog"
      :title="`编辑站点 ${editTarget.code}`"
      hint="提交时携带当前 version，落后会返回 VERSION_CONFLICT 并自动刷新最新版本"
      :fields="editFields"
      submit-label="保存修改"
      :loading="stations.saving"
      :error="stations.error"
      @submit="submitEdit"
      @cancel="editTarget = null"
    />

    <ConfirmDialog
      v-if="toggleTarget"
      :title="toggleTarget.enabled ? '停用站点' : '启用站点'"
      :hint="toggleHint"
      :danger="toggleTarget.enabled"
      :confirm-label="toggleTarget.enabled ? '确认停用' : '确认启用'"
      :loading="stations.saving"
      :error="stations.error"
      @confirm="submitToggle"
      @cancel="toggleTarget = null"
    />

    <FormDialog
      v-if="tariffOpen"
      test-id="tariff-create-dialog"
      title="新建基础价格版本"
      :fields="tariffFields"
      submit-label="创建版本"
      :loading="stations.saving"
      :error="stations.error"
      @submit="submitTariff"
      @cancel="tariffOpen = false"
    />

    <FormDialog
      v-if="adjustmentOpen"
      test-id="adjustment-dialog"
      title="批准服务费调整"
      hint="仅调整服务费，最终服务费必须限制在基础服务费的 80%~140%"
      :fields="adjustmentFields"
      submit-label="批准调整"
      :loading="stations.saving"
      :error="stations.error"
      @submit="submitAdjustment"
      @cancel="adjustmentOpen = false"
    />
  </div>
</template>

<style scoped>
.row-actions {
  display: inline-flex;
  gap: var(--ncs-s-2);
}

.pager {
  justify-content: flex-end;
  margin-top: var(--ncs-s-3);
}
</style>
