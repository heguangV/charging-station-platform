<script setup>
/**
 * 导航与品牌图标的唯一来源：统一的 24 栅格、1.7px 描边、currentColor 填充规则。
 * 用内联 SVG 取代 emoji，保证在 PC 侧边栏与移动端底部导航上都是同一套线性风格，
 * 且颜色随文字自动变化（选中态无需额外处理）。
 */
defineProps({
  /** home | charging | agent | orders | profile */
  name: { type: String, required: true },
  size: { type: [Number, String], default: 20 }
})
</script>

<template>
  <svg
    class="nav-icon"
    :width="size"
    :height="size"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="1.7"
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
    focusable="false"
  >
    <template v-if="name === 'home'">
      <path d="M12 21s6.5-5.4 6.5-10a6.5 6.5 0 1 0-13 0C5.5 15.6 12 21 12 21Z" />
      <circle cx="12" cy="10.6" r="2.4" />
    </template>

    <template v-else-if="name === 'charging'">
      <path d="M13.4 2.8 5.6 13.2h5l-1 8 7.8-10.4h-5l1-8Z" />
    </template>

    <template v-else-if="name === 'agent'">
      <path d="M12 3.2 13.7 8l4.8 1.7-4.8 1.7L12 16.2l-1.7-4.8L5.5 9.7 10.3 8 12 3.2Z" />
      <path d="M18.4 15.1l.7 1.9 1.9.7-1.9.7-.7 1.9-.7-1.9-1.9-.7 1.9-.7.7-1.9Z" />
    </template>

    <template v-else-if="name === 'orders'">
      <path d="M6.5 3h11a1 1 0 0 1 1 1v17l-3-2-3 2-3-2-3 2V4a1 1 0 0 1 1-1Z" />
      <path d="M9 8h6M9 12h6" />
    </template>

    <template v-else>
      <circle cx="12" cy="8.6" r="3.6" />
      <path d="M4.8 20.2a7.2 7.2 0 0 1 14.4 0" />
    </template>
  </svg>
</template>

<style scoped>
.nav-icon {
  display: block;
  flex: none;
}
</style>
