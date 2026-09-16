<script setup>
import { computed } from 'vue'
import { formatDistance, formatYuan } from '@/services/coordinate'

/**
 * 站点卡片。距离与价格均为显示层换算：
 * distanceMeter（整数米）→ “1.2 km”，totalPriceCentPerKwh（整数分/千瓦时）→ “1.35 元/kWh”。
 */
const props = defineProps({
  station: { type: Object, required: true },
  selected: { type: Boolean, default: false },
  clickable: { type: Boolean, default: true },
  /**
   * 站点列表 DTO 不包含分桩型统计，但按 chargerType 过滤时服务端只返回支持该桩型的站点，
   * 因此把当前过滤条件作为“支持快充/慢充”的补充证据。
   */
  matchedChargerType: { type: Number, default: null }
})

const emit = defineEmits(['select'])

const fastCount = computed(() => Number(props.station.fastChargerCount ?? 0))
const slowCount = computed(() => Number(props.station.slowChargerCount ?? 0))
const chargerTypes = computed(() => (Array.isArray(props.station.chargerTypes) ? props.station.chargerTypes : []))

const supportsFast = computed(
  () =>
    fastCount.value > 0 ||
    chargerTypes.value.some(type => type === 1 || type === 'DC_FAST' || type === 'FAST') ||
    props.matchedChargerType === 1
)
const supportsSlow = computed(
  () =>
    slowCount.value > 0 ||
    chargerTypes.value.some(type => type === 0 || type === 'AC_SLOW' || type === 'SLOW') ||
    props.matchedChargerType === 0
)

const distanceText = computed(() =>
  Number.isFinite(props.station.distanceMeter) ? formatDistance(props.station.distanceMeter) : '距离未知'
)

const priceText = computed(() => {
  const total = Number.isFinite(props.station.totalPriceCentPerKwh)
    ? props.station.totalPriceCentPerKwh
    : (props.station.electricityPriceCentPerKwh || 0) + (props.station.servicePriceCentPerKwh || 0)
  if (!Number.isFinite(total)) return '价格待更新'
  return `${formatYuan(total)} 元/kWh`
})

const idleCount = computed(() => Number(props.station.idleCount ?? 0))
const totalCount = computed(() => Number(props.station.totalCount ?? 0))

function select() {
  if (props.clickable) emit('select', props.station)
}
</script>

<template>
  <article
    class="station-card"
    :class="{ 'is-selected': selected, 'is-clickable': clickable }"
    data-testid="station-card"
    :data-station-id="station.id"
    :role="clickable ? 'button' : undefined"
    :tabindex="clickable ? 0 : undefined"
    @click="select"
    @keydown.enter.prevent="select"
    @keydown.space.prevent="select"
  >
    <header class="station-card__head">
      <h3 class="station-card__name" data-testid="station-card-name">{{ station.name }}</h3>
      <span class="station-card__distance" data-testid="station-card-distance">{{ distanceText }}</span>
    </header>

    <p class="station-card__address" data-testid="station-card-address">{{ station.address || '地址待补充' }}</p>

    <div class="station-card__tags">
      <span v-if="supportsFast" class="tag tag--fast" data-testid="station-card-fast">
        快充{{ fastCount > 0 ? ` ${fastCount} 空闲` : '' }}
      </span>
      <span v-if="supportsSlow" class="tag tag--slow" data-testid="station-card-slow">
        慢充{{ slowCount > 0 ? ` ${slowCount} 空闲` : '' }}
      </span>
      <span class="tag" data-testid="station-card-operational">可用桩 {{ station.operationalCount ?? 0 }}</span>
    </div>

    <footer class="station-card__foot">
      <div>
        <p class="station-card__price" data-testid="station-card-price">{{ priceText }}</p>
        <p class="station-card__counts" data-testid="station-card-counts">
          空闲 <strong data-testid="station-card-idle">{{ idleCount }}</strong> / 共
          <strong data-testid="station-card-total">{{ totalCount }}</strong>
        </p>
      </div>
      <button type="button" class="btn btn--ghost" data-testid="station-card-select" @click.stop="select">查看详情</button>
    </footer>
  </article>
</template>

<style scoped>
/* 站点卡片：以发丝描边 + 柔和投影建立层次，悬停时轻微上浮，按下时回落，形成“可按”的手感。 */
.station-card {
  position: relative;
  display: flex;
  flex-direction: column;
  gap: var(--ncs-s-3);
  padding: var(--ncs-s-4);
  background: var(--ncs-surface);
  border: 1px solid var(--ncs-line);
  border-radius: var(--ncs-r-lg);
  box-shadow: var(--ncs-shadow-1);
  transition:
    transform var(--ncs-dur-2) var(--ncs-ease-out),
    box-shadow var(--ncs-dur-2) var(--ncs-ease-out),
    border-color var(--ncs-dur-2) var(--ncs-ease-out);
}

/* 左侧品牌指示条：选中时自下而上展开 */
.station-card::before {
  content: '';
  position: absolute;
  left: 0;
  top: 14px;
  bottom: 14px;
  width: 3px;
  border-radius: 0 3px 3px 0;
  background: linear-gradient(180deg, var(--ncs-brand-bright), var(--ncs-brand-strong));
  transform: scaleY(0);
  transform-origin: top;
  transition: transform var(--ncs-dur-3) var(--ncs-ease-spring);
}

.station-card.is-clickable {
  cursor: pointer;
}

.station-card.is-clickable:hover {
  transform: translate3d(0, -3px, 0);
  box-shadow: var(--ncs-shadow-2);
  border-color: var(--ncs-line-strong);
}

.station-card.is-clickable:active {
  transform: translate3d(0, -1px, 0) scale(0.995);
  box-shadow: var(--ncs-shadow-1);
}

.station-card.is-selected {
  border-color: rgba(13, 122, 111, 0.45);
  box-shadow: var(--ncs-shadow-2);
  background: linear-gradient(180deg, rgba(13, 122, 111, 0.035), transparent 40%);
}

.station-card.is-selected::before {
  transform: scaleY(1);
}

.station-card__head {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: var(--ncs-s-3);
}

.station-card__name {
  font-size: var(--ncs-fs-base);
  font-weight: 650;
  margin: 0;
  letter-spacing: -0.01em;
}

.station-card__distance {
  flex: none;
  color: var(--ncs-brand-strong);
  font-size: var(--ncs-fs-sm);
  font-weight: 600;
  white-space: nowrap;
  font-variant-numeric: tabular-nums;
  padding: 2px 10px;
  border-radius: var(--ncs-r-pill);
  background: var(--ncs-brand-softer);
}

.station-card__address {
  margin: 0;
  color: var(--ncs-muted);
  font-size: var(--ncs-fs-sm);
  display: flex;
  align-items: center;
  gap: 6px;
}

.station-card__address::before {
  content: '';
  flex: none;
  width: 12px;
  height: 12px;
  background: currentColor;
  opacity: 0.5;
  mask: radial-gradient(circle at 50% 40%, transparent 3px, #000 3.5px) no-repeat center / 12px 12px;
  -webkit-mask: radial-gradient(circle at 50% 40%, transparent 3px, #000 3.5px) no-repeat center / 12px 12px;
}

.station-card__tags {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
}

.tag {
  padding: 3px 10px;
  border-radius: var(--ncs-r-pill);
  font-size: var(--ncs-fs-xs);
  font-weight: 500;
  background: var(--ncs-surface-3);
  color: var(--ncs-muted);
  border: 1px solid transparent;
  transition:
    background var(--ncs-dur-2) var(--ncs-ease-out),
    color var(--ncs-dur-2) var(--ncs-ease-out);
}

.tag--fast {
  background: var(--ncs-accent-soft);
  color: #7a4a06;
}

.tag--slow {
  background: var(--ncs-brand-soft);
  color: var(--ncs-brand-strong);
}

.station-card__foot {
  display: flex;
  align-items: flex-end;
  justify-content: space-between;
  gap: var(--ncs-s-3);
  padding-top: var(--ncs-s-3);
  border-top: 1px solid var(--ncs-line);
}

.station-card__price {
  margin: 0;
  font-size: var(--ncs-fs-md);
  font-weight: 700;
  letter-spacing: -0.01em;
  color: var(--ncs-brand-strong);
}

.station-card__counts {
  margin: 2px 0 0;
  font-size: var(--ncs-fs-sm);
  color: var(--ncs-muted);
}

.station-card__counts strong {
  color: var(--ncs-text);
  font-weight: 650;
}
</style>
