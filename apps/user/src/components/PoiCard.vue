<script setup>
import { computed } from 'vue'
import { formatDistance } from '@/services/coordinate'

/** POI 卡片：AI 助手返回的周边兴趣点，可选导航入口（在新标签页打开腾讯地图 URL）。 */
const props = defineProps({
  poi: { type: Object, required: true },
  url: { type: String, default: '' }
})

const emit = defineEmits(['navigate'])

const distanceText = computed(() =>
  Number.isFinite(props.poi.distanceMeter) ? formatDistance(props.poi.distanceMeter) : '距离未知'
)

function navigate(event) {
  if (props.url) emit('navigate', { url: props.url, poi: props.poi })
  else event.preventDefault()
}
</script>

<template>
  <article class="poi-card" data-testid="poi-card" :data-poi-id="poi.id">
    <header class="poi-card__head">
      <h4 class="poi-card__name" data-testid="poi-card-name">{{ poi.name }}</h4>
      <span class="poi-card__distance" data-testid="poi-card-distance">{{ distanceText }}</span>
    </header>
    <p class="poi-card__category" data-testid="poi-card-category">{{ poi.category || '周边' }}</p>
    <p class="poi-card__address" data-testid="poi-card-address">{{ poi.address || '地址待补充' }}</p>
    <footer class="poi-card__foot">
      <span v-if="poi.tel" class="poi-card__tel">电话 {{ poi.tel }}</span>
      <a
        v-if="url"
        class="btn btn--ghost"
        data-testid="poi-card-navigate"
        :href="url"
        target="_blank"
        rel="noopener noreferrer"
        @click="navigate"
        >导航</a
      >
    </footer>
  </article>
</template>

<style scoped>
/* POI 卡片：比站点卡片更轻，用同一条左侧色条区分“地点”而非“场站”。 */
.poi-card {
  position: relative;
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding: var(--ncs-s-3) var(--ncs-s-4);
  background: var(--ncs-surface);
  border: 1px solid var(--ncs-line);
  border-radius: var(--ncs-r-md);
  box-shadow: var(--ncs-shadow-1);
  transition:
    transform var(--ncs-dur-2) var(--ncs-ease-out),
    box-shadow var(--ncs-dur-2) var(--ncs-ease-out),
    border-color var(--ncs-dur-2) var(--ncs-ease-out);
}

.poi-card::before {
  content: '';
  position: absolute;
  left: 0;
  top: 12px;
  bottom: 12px;
  width: 3px;
  border-radius: 0 3px 3px 0;
  background: linear-gradient(180deg, var(--ncs-accent), rgba(224, 138, 30, 0.5));
}

.poi-card:hover {
  transform: translate3d(0, -2px, 0);
  box-shadow: var(--ncs-shadow-2);
  border-color: var(--ncs-line-strong);
}

.poi-card__head {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: var(--ncs-s-2);
}

.poi-card__name {
  margin: 0;
  font-size: var(--ncs-fs-base);
  font-weight: 600;
  letter-spacing: -0.01em;
}

.poi-card__distance {
  flex: none;
  font-size: var(--ncs-fs-xs);
  font-weight: 600;
  color: var(--ncs-brand-strong);
  font-variant-numeric: tabular-nums;
}

.poi-card__category,
.poi-card__address {
  margin: 0;
  font-size: var(--ncs-fs-sm);
  color: var(--ncs-muted);
}

.poi-card__category {
  display: inline-flex;
  align-self: flex-start;
  padding: 2px 8px;
  border-radius: var(--ncs-r-pill);
  background: var(--ncs-accent-soft);
  color: #7a4a06;
  font-size: var(--ncs-fs-xs);
}

.poi-card__foot {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--ncs-s-3);
  margin-top: var(--ncs-s-2);
}

.poi-card__tel {
  font-size: var(--ncs-fs-xs);
  color: var(--ncs-muted);
  font-variant-numeric: tabular-nums;
}
</style>
