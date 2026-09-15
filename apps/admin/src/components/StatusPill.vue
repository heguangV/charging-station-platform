<script setup>
/**
 * 状态胶囊：统一展示服务端返回的 statusText 与语义色。
 * 色调只影响颜色，不改变任何文字，因此断言的文案始终来自服务端。
 */
defineProps({
  text: { type: String, default: '—' },
  /** ok | warn | danger | info | muted */
  tone: { type: String, default: 'muted' },
  testId: { type: String, default: null }
})
</script>

<template>
  <span class="status-pill" :data-tone="tone" :data-testid="testId">{{ text }}</span>
</template>

<style scoped>
.status-pill {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  padding: 3px 10px;
  border-radius: var(--ncs-r-pill);
  font-size: var(--ncs-fs-xs);
  font-weight: 600;
  white-space: nowrap;
  background: var(--ncs-surface-3);
  color: var(--ncs-text-2);
  transition: background var(--ncs-dur-2) var(--ncs-ease-out);
}

.status-pill::before {
  content: '';
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: currentColor;
  opacity: 0.85;
}

.status-pill[data-tone='ok'] {
  background: var(--ncs-brand-soft);
  color: var(--ncs-brand-strong);
}

.status-pill[data-tone='info'] {
  background: var(--ncs-info-soft);
  color: #265ca8;
}

.status-pill[data-tone='warn'] {
  background: var(--ncs-accent-soft);
  color: #8a5306;
}

.status-pill[data-tone='danger'] {
  background: var(--ncs-danger-soft);
  color: var(--ncs-danger);
}

.status-pill[data-tone='muted'] {
  background: var(--ncs-surface-3);
  color: var(--ncs-muted);
}
</style>
