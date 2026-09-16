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
import { CHARGER_TYPES, ADJUSTMENT_SOURCES } from '@/utils/domain'
import { formatAmount, formatDateTime, formatInt, fromDateTimeInputValue } from '@/utils/format'

const stations = useStationsStore()
const auth = useAuthStore()

const keyword = ref('')

const createOpen = ref(false)
const editTarget = ref(null)
const toggleTarget = ref(null)
const tariffOpen = ref(false)
const adjustmentOpen = ref(false)

onMounted(async () => {
  await stations.load()
  await stations.loadTariffs()
})

/**
 * 列与 Go Station 契约一一对应：编码、名称、运营状态，末列放行内操作。
 *
 * 行政区编码与版本在 Go 契约里没有对应字段，列出来只会是「—」；行内操作
 * 原本挂在「版本」列上，现在挂到诚实的「操作」列。
 */
const stationColumns = [
  { key: 'code', label: '站点编码' },
  { key: 'name', label: '站点名称' },
  { key: 'enabled', label: '运营状态', format: (value, row) => stationStateText(row) },
  { key: 'actions', label: '操作', align: 'right' }
]

const tariffColumns = [
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
  {
    key: 'offPeakElectricityPriceCentPerKwh',
    label: '谷时电费',
    align: 'right',
    format: (value, row) => {
      if (!Number.isFinite(value)) return '单一费率'
      const window = `${String(row.offPeakStartHour ?? 0).padStart(2, '0')}:00-${String(row.offPeakEndHour ?? 0).padStart(2, '0')}:00`
      return `${formatAmount(value)} 元（${window}）`
    }
  },
  { key: 'chargerCount', label: '设备数', align: 'right', format: value => formatInt(value) }
]

/**
 * 新增站点：字段就是 Go CreateStationRequest 的全部五个。
 *
 * 行政区编码、营业时间、初始设备（数量/类型/功率/接口标准）在 Go 契约里都不存在，
 * 收进表单只会在提交时被静默丢掉，让管理员以为填了就算数。
 * 上限按契约取（编码 2~32、名称 100、地址 255），避免表单比服务端更严。
 * 经纬度按十进制度输入，提交前换算为整数 E6 度。
 */
const createFields = [
  { key: 'code', label: '站点编码', required: true, minLength: 2, maxLength: 32, placeholder: '如 ZGC2' },
  { key: 'name', label: '站点名称', required: true, maxLength: 100 },
  { key: 'address', label: '地址', required: true, maxLength: 255 },
  { key: 'latitude', label: '纬度（度）', type: 'number', integer: false, required: true, min: -90, max: 90 },
  { key: 'longitude', label: '经度（度）', type: 'number', integer: false, required: true, min: -180, max: 180 }
]

/**
 * 可编辑字段就是服务端接受的四个：名称、地址与经纬度。
 *
 * 行政区编码与营业时间在 Go 契约里没有对应列，放进表单只会让管理员以为
 * 自己改动的字段被保存了。
 */
const editFields = computed(() => [
  { key: 'name', label: '站点名称', required: true, maxLength: 100 },
  { key: 'address', label: '地址', required: true, maxLength: 255 },
  { key: 'latitude', label: '纬度（度）', type: 'number', integer: false, required: true, min: -90, max: 90 },
  { key: 'longitude', label: '经度（度）', type: 'number', integer: false, required: true, min: -180, max: 180 }
])

/**
 * 全局费率下发：请求体是完整费率，不填谷时三项即表示全车队改为单一费率。
 * 原因必填，会写入审计。
 */
const tariffFields = [
  { key: 'electricityPriceCentPerKwh', label: '电费（分/kWh）', type: 'number', required: true, min: 0, max: 100000 },
  { key: 'servicePriceCentPerKwh', label: '服务费（分/kWh）', type: 'number', required: true, min: 0, max: 100000 },
  {
    key: 'offPeakElectricityPriceCentPerKwh',
    label: '谷时电费（分/kWh，可空）',
    type: 'number',
    min: 0,
    max: 100000,
    help: '留空表示全车队改为单一费率；填写则必须同时填谷时起止小时'
  },
  { key: 'offPeakStartHour', label: '谷时开始小时（0-23）', type: 'number', min: 0, max: 23 },
  { key: 'offPeakEndHour', label: '谷时结束小时（0-23）', type: 'number', min: 0, max: 23 },
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
    latitudeE6: toE6(values.latitude),
    longitudeE6: toE6(values.longitude)
  })
  if (ok) createOpen.value = false
}

async function submitEdit(values) {
  if (!editTarget.value) return
  const ok = await stations.edit(editTarget.value, {
    name: values.name,
    address: values.address,
    latitudeE6: toE6(values.latitude),
    longitudeE6: toE6(values.longitude)
  })
  if (ok) editTarget.value = null
}

async function submitToggle({ reason }) {
  if (!toggleTarget.value) return
  const ok = await stations.setEnabled(toggleTarget.value, !toggleTarget.value.enabled, reason)
  if (ok) toggleTarget.value = null
}

async function submitTariff(values) {
  // 谷时三项要么全给要么全不给：服务端拒绝半个窗口，这里先按同一规则整理。
  const hasOffPeakWindow = values.offPeakElectricityPriceCentPerKwh !== undefined &&
    values.offPeakElectricityPriceCentPerKwh !== '' &&
    values.offPeakElectricityPriceCentPerKwh !== null
  const payload = {
    electricityPriceCentPerKwh: values.electricityPriceCentPerKwh,
    servicePriceCentPerKwh: values.servicePriceCentPerKwh,
    reason: values.reason
  }
  if (hasOffPeakWindow) {
    payload.offPeakElectricityPriceCentPerKwh = values.offPeakElectricityPriceCentPerKwh
    payload.offPeakStartHour = values.offPeakStartHour
    payload.offPeakEndHour = values.offPeakEndHour
  }
  const ok = await stations.setGlobalTariff(payload)
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
  stations.setFilter({ keyword: keyword.value })
}

function resetFilters() {
  keyword.value = ''
  stations.resetFilters()
}

/**
 * Go 契约只给 status（OPEN/CLOSED/DISABLED），没有 enabled 布尔列；
 * enabled 由 api/station.js 在列表与状态变更两处统一换算，页面沿用两态展示。
 */
function isStationOpen(row) {
  return row?.enabled === true
}

function stationStateText(row) {
  return isStationOpen(row) ? '运营中' : '已停用'
}

function statusTone(row) {
  return isStationOpen(row) ? 'ok' : 'muted'
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
    <p v-if="!auth.canWrite" class="alert" data-testid="stations-permission-hint">
      当前账号为只读角色（{{ auth.roles.join(' / ') || '未识别' }}）：站点编辑、启停与费率写入需要 SUPER_ADMIN 或 OPERATOR 权限。
    </p>

    <section class="panel" v-reveal>
      <div class="panel__title">
        <div>
          <h2>站点检索</h2>
          <p class="panel__hint">Go 列表契约只支持名称/地址关键词 + 分页，因此这里只提供关键词检索</p>
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
          <StatusPill :text="stationStateText(row)" :tone="statusTone(row)" :test-id="`station-state-${row.id}`" />
        </template>
        <template #cell-actions="{ row }">
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
          <h2>全局费率</h2>
          <p class="panel__hint">费率存在电桩上：下面是全车队当前的费率构成；下发会一次写入所有设备并记录审计，已生成订单的价格快照不受影响</p>
        </div>
        <div class="panel__row">
          <button type="button" class="btn btn--sm" data-testid="tariff-create" @click="tariffOpen = true">
            统一下发费率
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
        row-key="key"
        :loading="stations.tariffsLoading"
        :error="stations.tariffsError"
        empty-text="暂无设备费率记录"
      />
    </section>

    <FormDialog
      v-if="createOpen"
      test-id="station-create-dialog"
      title="新增站点"
      hint="Go 契约一次只创建站点本身（编码/名称/地址/经纬度）；初始设备请在充电桩页新增"
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
      hint="仅可修改名称、地址与经纬度；站点编码与运营状态不在此处，状态用下方的启停操作"
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
      title="统一下发费率"
      hint="一次写入全部电桩（含停用设备）；留空谷时字段即改为单一费率"
      :fields="tariffFields"
      submit-label="下发费率"
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
