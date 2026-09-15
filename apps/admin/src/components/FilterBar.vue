<script setup>
/**
 * 筛选条：面板内的表单行，字段由调用方以 v-model 绑定后放进默认插槽，
 * 右侧操作区走 actions 插槽。提交走 submit 事件（支持回车提交）。
 */
defineProps({
  testId: { type: String, required: true },
  busy: { type: Boolean, default: false }
})

defineEmits(['submit'])
</script>

<template>
  <form class="filter-bar" :data-testid="testId" @submit.prevent="$emit('submit')">
    <div class="filter-bar__fields">
      <slot />
    </div>
    <div class="filter-bar__actions">
      <button type="submit" class="btn btn--primary" :disabled="busy">查询</button>
      <slot name="actions" />
    </div>
  </form>
</template>

<style scoped>
.filter-bar {
  display: flex;
  flex-wrap: wrap;
  align-items: flex-end;
  gap: var(--ncs-s-3);
  width: 100%;
}

.filter-bar__fields {
  display: flex;
  flex-wrap: wrap;
  gap: var(--ncs-s-3);
  flex: 1;
  min-width: 220px;
}

.filter-bar__fields :deep(.field) {
  min-width: 150px;
  flex: 1;
}

.filter-bar__actions {
  display: flex;
  align-items: center;
  gap: var(--ncs-s-2);
}
</style>
