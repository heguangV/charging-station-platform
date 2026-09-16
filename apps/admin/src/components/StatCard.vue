<script setup>
/**
 * 指标卡：数字由 format 决定展示方式（金额分→元、百分比、台数），
 * 数值发生变化时通过 useValueFlash 加一次 .value-flash 高亮，
 * 首次渲染不闪烁，且动画只改 class、绝不改文本，读数始终是最终值。
 */
import { computed } from 'vue'
import { useValueFlash } from '@/composables/useValueFlash'
import { EMPTY, formatAmount, formatEnergy, formatInt, formatPercent } from '@/utils/format'
import AppSkeleton from './AppSkeleton.vue'

const props = defineProps({
  label: { type: String, required: true },
  value: { type: [Number, String], default: null },
  /** amount | percent | count | energy | raw */
  format: { type: String, default: 'raw' },
  digits: { type: Number, default: null },
  hint: { type: String, default: '' },
  testId: { type: String, required: true },
  loading: { type: Boolean, default: false },
  tone: { type: String, default: 'brand' }
})

/** 展示文本：所有分支都保证不会出现 NaN / undefined。 */
const text = computed(() => {
  if (props.value === null || props.value === undefined || props.value === '') return EMPTY
  switch (props.format) {
    case 'amount':
      return formatAmount(props.value, { digits: props.digits ?? 2 })
    case 'percent':
      return formatPercent(props.value, { digits: props.digits ?? 1 })
    case 'energy':
      return formatEnergy(props.value, { digits: props.digits ?? 2 })
    case 'count':
      return formatInt(props.value)
    default:
      return String(props.value)
  }
})

const { flashing } = useValueFlash(() => text.value)
</script>

<template>
  <div class="stat-card" :data-testid="testId" :data-tone="tone">
    <p class="stat-card__label">{{ label }}</p>
    <AppSkeleton v-if="loading" :data-testid="`${testId}-loading`" variant="lines" :rows="1" />
    <p
      v-else
      class="stat-card__value num"
      :class="{ 'value-flash': flashing }"
      :data-testid="`${testId}-value`"
    >
      {{ text }}
    </p>
    <p v-if="hint" class="stat-card__hint">{{ hint }}</p>
  </div>
</template>

<style scoped>
.stat-card {
  position: relative;
  display: flex;
  flex-direction: column;
  gap: var(--ncs-s-1);
  padding: var(--ncs-s-4);
  background: var(--ncs-surface);
  border: 1px solid var(--ncs-line);
  border-radius: var(--ncs-r-lg);
  box-shadow: var(--ncs-shadow-1);
  overflow: hidden;
  transition:
    box-shadow var(--ncs-dur-2) var(--ncs-ease-out),
    transform var(--ncs-dur-2) var(--ncs-ease-out);
}

.stat-card:hover {
  box-shadow: var(--ncs-shadow-2);
  transform: translateY(-1px);
}

/* 左侧强调条：品牌色随 tone 变化，保持单一强调色语言 */
.stat-card::before {
  content: '';
  position: absolute;
  left: 0;
  top: var(--ncs-s-4);
  bottom: var(--ncs-s-4);
  width: 3px;
  border-radius: 0 3px 3px 0;
  background: linear-gradient(180deg, var(--ncs-brand-bright), var(--ncs-brand));
}

.stat-card[data-tone='accent']::before {
  background: linear-gradient(180deg, #f0b458, var(--ncs-accent));
}

.stat-card[data-tone='danger']::before {
  background: linear-gradient(180deg, #e0655c, var(--ncs-danger));
}

.stat-card__label {
  margin: 0;
  font-size: var(--ncs-fs-xs);
  color: var(--ncs-muted);
  letter-spacing: 0.04em;
}

.stat-card__value {
  margin: 0;
  font-size: var(--ncs-fs-2xl);
  font-weight: 660;
  letter-spacing: -0.02em;
  line-height: 1.15;
}

.stat-card__hint {
  margin: 0;
  font-size: var(--ncs-fs-xs);
  color: var(--ncs-muted);
}
</style>
