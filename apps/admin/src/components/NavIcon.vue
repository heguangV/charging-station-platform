<script setup>
/**
 * 管理端图标的唯一来源：统一的 24 栅格、1.7px 描边、currentColor 描边色。
 * 用内联 SVG 取代 emoji，侧栏、顶栏与表格操作按钮共用同一套线性风格，
 * 颜色随文字自动变化，选中态不需要额外处理。
 */
defineProps({
  /** overview | station | charger | user | flow | prediction | account | ops | menu | close | refresh | power | logout */
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
    <template v-if="name === 'overview'">
      <rect x="3.5" y="3.5" width="7" height="7" rx="2" />
      <rect x="13.5" y="3.5" width="7" height="7" rx="2" />
      <rect x="3.5" y="13.5" width="7" height="7" rx="2" />
      <rect x="13.5" y="13.5" width="7" height="7" rx="2" />
    </template>

    <template v-else-if="name === 'station'">
      <path d="M6 21V5.5A1.5 1.5 0 0 1 7.5 4h7A1.5 1.5 0 0 1 16 5.5V21" />
      <path d="M4 21h16M9 8h4M9 12h4M16 10h1.6a1.4 1.4 0 0 1 1.4 1.4V15" />
      <circle cx="19" cy="16.6" r="1.6" />
    </template>

    <template v-else-if="name === 'charger'">
      <rect x="5.5" y="3.5" width="10" height="17" rx="2.2" />
      <path d="M13.6 7.2 9.3 12.4h2.9l-1 4.4 4.3-5.3h-2.9l1-4.3Z" />
      <path d="M15.5 8h2.2a1.3 1.3 0 0 1 1.3 1.3v6.1" />
    </template>

    <template v-else-if="name === 'user'">
      <circle cx="12" cy="8.4" r="3.6" />
      <path d="M4.8 20.2a7.2 7.2 0 0 1 14.4 0" />
    </template>

    <template v-else-if="name === 'flow'">
      <path d="M4 7h9a4 4 0 0 1 4 4 4 4 0 0 1-4 4H8" />
      <path d="M6.5 4.5 4 7l2.5 2.5" />
      <path d="M17.5 19.5 20 17l-2.5-2.5" />
      <path d="M20 17h-9" />
    </template>

    <template v-else-if="name === 'prediction'">
      <path d="M4 19.5h16" />
      <path d="M4 19.5V4.5" />
      <path d="M6.5 15.5 10.5 10l3.2 2.6L19.5 6" />
      <path d="M16 6h3.5v3.5" />
    </template>

    <template v-else-if="name === 'account'">
      <path d="M12 3.2 4.8 6v5.4c0 4.2 3 7.6 7.2 9.4 4.2-1.8 7.2-5.2 7.2-9.4V6L12 3.2Z" />
      <path d="M9.6 12.1l1.8 1.8 3.2-3.6" />
    </template>

    <template v-else-if="name === 'ops'">
      <ellipse cx="12" cy="6" rx="7" ry="2.8" />
      <path d="M5 6v6c0 1.5 3.1 2.8 7 2.8s7-1.3 7-2.8V6" />
      <path d="M5 12v6c0 1.5 3.1 2.8 7 2.8s7-1.3 7-2.8v-6" />
    </template>

    <template v-else-if="name === 'menu'">
      <path d="M4 7h16M4 12h16M4 17h16" />
    </template>

    <template v-else-if="name === 'close'">
      <path d="M6.5 6.5 17.5 17.5M17.5 6.5 6.5 17.5" />
    </template>

    <template v-else-if="name === 'refresh'">
      <path d="M19.5 12a7.5 7.5 0 1 1-2.4-5.5" />
      <path d="M19.5 4.5V9H15" />
    </template>

    <template v-else-if="name === 'power'">
      <path d="M12 3.5v7" />
      <path d="M6.9 6.6a7.5 7.5 0 1 0 10.2 0" />
    </template>

    <template v-else-if="name === 'logout'">
      <path d="M14.5 4.5h3.2A1.3 1.3 0 0 1 19 5.8v12.4a1.3 1.3 0 0 1-1.3 1.3h-3.2" />
      <path d="M11 8.5 7.5 12 11 15.5" />
      <path d="M7.8 12H15" />
    </template>

    <template v-else>
      <circle cx="12" cy="12" r="8" />
      <path d="M12 8.4v4.2l2.6 1.6" />
    </template>
  </svg>
</template>

<style scoped>
.nav-icon {
  display: block;
  flex: none;
}
</style>
