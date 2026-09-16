<script setup>
/**
 * 加载骨架屏：用与真实内容一致的版式占位，避免“加载中”文字造成的布局跳动。
 * 纯装饰元素（aria-hidden），不携带任何可断言的业务文案；
 * 承载它的容器仍保留原有 data-testid，因此既有加载态断言不受影响。
 */
defineProps({
  /** cards：卡片列表；lines：若干文本行；table：表格行。 */
  variant: { type: String, default: 'cards' },
  rows: { type: Number, default: 3 }
})

/** 固定的一组宽度比例，做出参差感且每次渲染一致，不会闪烁变化。 */
const WIDTHS = ['82%', '64%', '74%', '58%', '70%', '52%']
</script>

<template>
  <div class="skeleton-block" :data-skeleton="variant" aria-hidden="true">
    <template v-if="variant === 'cards'">
      <div v-for="index in rows" :key="index" class="skeleton-card overlay-fade">
        <div class="skeleton skeleton-line skeleton-line--title"></div>
        <div class="skeleton skeleton-line" :style="{ width: WIDTHS[(index - 1) % WIDTHS.length] }"></div>
        <div class="skeleton skeleton-line skeleton-line--short"></div>
      </div>
    </template>

    <template v-else-if="variant === 'table'">
      <div v-for="index in rows" :key="index" class="skeleton skeleton-line skeleton-line--row"></div>
    </template>

    <template v-else>
      <div
        v-for="index in rows"
        :key="index"
        class="skeleton skeleton-line"
        :style="{ width: WIDTHS[(index - 1) % WIDTHS.length] }"
      ></div>
    </template>
  </div>
</template>

<style scoped>
.skeleton-block {
  display: flex;
  flex-direction: column;
  gap: var(--ncs-s-3);
}

.skeleton-line--title {
  width: 46%;
  height: 15px;
}

.skeleton-line--short {
  width: 34%;
  height: 10px;
}

.skeleton-line--row {
  height: 34px;
  border-radius: var(--ncs-r-sm);
}
</style>
