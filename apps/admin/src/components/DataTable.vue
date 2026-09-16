<script setup>
/**
 * 数据表格：加载骨架、空状态、错误重试与行选中都在这里统一实现，
 * 各页面只提供列定义与行数据，避免每页重复一套状态分支。
 *
 * 取值约定：`column.format(value, row)` 优先；其余情况由 cellText 兜底，
 * 缺失字段、null、空串、NaN 一律显示 '—'，绝不把 NaN 渲染到界面上。
 * 所有行的 data-testid 由 `rowTestId` 生成（例如 station-row-3）。
 */
import AppSkeleton from './AppSkeleton.vue'
import { EMPTY } from '@/utils/format'

const props = defineProps({
  /** [{ key, label, align, width, format?, value? }] */
  columns: { type: Array, required: true },
  rows: { type: Array, default: () => [] },
  /** 行主键字段名，或 (row) => key 的函数。 */
  rowKey: { type: [String, Function], default: 'id' },
  loading: { type: Boolean, default: false },
  error: { type: String, default: '' },
  emptyText: { type: String, default: '暂无数据' },
  /** 页面标识：派生 `<testId>-table/-loading/-error/-empty`。 */
  testId: { type: String, required: true },
  /** (row) => data-testid 字符串。 */
  rowTestId: { type: Function, default: null },
  selectedKey: { type: [String, Number], default: null },
  skeletonRows: { type: Number, default: 5 }
})

defineEmits(['row-click', 'retry'])

function keyOf(row) {
  if (typeof props.rowKey === 'function') return props.rowKey(row)
  return row ? row[props.rowKey] : null
}

function rawValue(row, column) {
  if (typeof column.value === 'function') return column.value(row)
  return row ? row[column.key] : null
}

/**
 * 单元格文本：格式化函数优先，其余按类型安全转换。
 * 值为空时**不调用**格式化函数——列格式化器通常做算术（如 mWh/1000000），
 * 对缺失值运算会得到 NaN；这里统一拦截为占位符。
 */
function cellText(row, column) {
  const value = rawValue(row, column)
  if (value === null || value === undefined || value === '') return EMPTY
  if (typeof column.format === 'function') {
    const text = column.format(value, row)
    return text === null || text === undefined || text === '' ? EMPTY : String(text)
  }
  if (value === null || value === undefined || value === '') return EMPTY
  if (typeof value === 'number') return Number.isFinite(value) ? String(value) : EMPTY
  if (typeof value === 'boolean') return value ? '是' : '否'
  return String(value)
}

function rowTestIdOf(row) {
  if (typeof props.rowTestId === 'function') return props.rowTestId(row)
  return `${props.testId}-row-${keyOf(row)}`
}
</script>

<template>
  <div class="data-table-wrap" :data-testid="`${testId}-table`">
    <!-- 加载骨架沿用页面级 testid：加载态与就绪态指向同一个容器语义。 -->
    <AppSkeleton v-if="loading" :data-testid="`${testId}-loading`" variant="table" :rows="skeletonRows" />

    <p v-else-if="error" class="alert alert--error" :data-testid="`${testId}-error`">
      {{ error }}
      <span class="alert__actions">
        <button type="button" class="btn btn--sm" @click="$emit('retry')">重试</button>
      </span>
    </p>

    <p v-else-if="rows.length === 0" class="empty-state" :data-testid="`${testId}-empty`">
      {{ emptyText }}
    </p>

    <div v-else class="table-scroll">
      <table class="data-table">
        <thead>
          <tr>
            <th
              v-for="column in columns"
              :key="column.key"
              :class="{ 'is-num': column.align === 'right' }"
              :style="column.width ? { width: column.width } : null"
              scope="col"
            >
              {{ column.label }}
            </th>
          </tr>
        </thead>
        <TransitionGroup name="list" tag="tbody">
          <tr
            v-for="row in rows"
            :key="keyOf(row)"
            :data-testid="rowTestIdOf(row)"
            :class="{ 'is-selected': selectedKey !== null && keyOf(row) === selectedKey }"
            @click="$emit('row-click', row)"
          >
            <td
              v-for="column in columns"
              :key="column.key"
              :class="{ 'is-num': column.align === 'right' }"
              :title="cellText(row, column)"
            >
              <slot :name="`cell-${column.key}`" :row="row" :value="rawValue(row, column)" :text="cellText(row, column)">
                {{ cellText(row, column) }}
              </slot>
            </td>
          </tr>
        </TransitionGroup>
      </table>
    </div>
  </div>
</template>

<style scoped>
.data-table-wrap {
  width: 100%;
}
</style>
