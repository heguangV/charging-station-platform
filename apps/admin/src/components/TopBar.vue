<script setup>
/**
 * 吸顶顶栏：左侧是抽屉开关（仅窄屏可见）与面包屑/页面标题，右侧是管理员身份与退出。
 * 使用毛玻璃背景 + 底部品牌渐变细线，与用户端保持同一视觉语言。
 */
import { computed } from 'vue'
import NavIcon from './NavIcon.vue'
import { useAuthStore } from '@/stores/auth'

const props = defineProps({
  title: { type: String, default: '运营总览' },
  crumb: { type: String, default: 'NCS 充电运营中心' }
})

defineEmits(['toggle-rail', 'logout'])

const auth = useAuthStore()

/** 角色只做展示；权限判定始终以服务端响应为准。 */
const roleLabel = computed(() => {
  const roles = auth.roles
  if (roles.includes('SUPER_ADMIN')) return '超级管理员'
  if (roles.includes('OPERATOR')) return '运营管理员'
  if (roles.includes('AUDITOR')) return '审计员'
  return '未识别角色'
})

const identity = computed(() => auth.displayName)
</script>

<template>
  <header class="top-bar" data-testid="top-bar">
    <button
      type="button"
      class="btn btn--sm rail-toggle"
      aria-label="打开导航"
      data-testid="rail-open"
      @click="$emit('toggle-rail')"
    >
      <NavIcon name="menu" :size="16" />
    </button>

    <div class="top-bar__heading">
      <p class="top-bar__crumb">{{ props.crumb }}</p>
      <h1 class="top-bar__title" data-testid="page-title">{{ props.title }}</h1>
    </div>

    <div class="top-bar__actions">
      <span class="top-bar__identity" data-testid="admin-identity">
        <span class="top-bar__name">{{ identity }}</span>
        <span class="top-bar__role">{{ roleLabel }}</span>
      </span>
      <RouterLink class="btn btn--sm" to="/accounts" data-testid="top-bar-account">
        <NavIcon name="account" :size="15" />
        <span class="top-bar__label">账号</span>
      </RouterLink>
      <button type="button" class="btn btn--sm btn--danger" data-testid="admin-logout" @click="$emit('logout')">
        <NavIcon name="logout" :size="15" />
        <span class="top-bar__label">退出</span>
      </button>
    </div>
  </header>
</template>

<style scoped>
.top-bar__identity {
  display: flex;
  flex-direction: column;
  align-items: flex-end;
  line-height: 1.25;
  padding-right: var(--ncs-s-1);
}

.top-bar__name {
  font-size: var(--ncs-fs-sm);
  font-weight: 650;
}

.top-bar__role {
  font-size: var(--ncs-fs-xs);
  color: var(--ncs-muted);
}

@media (max-width: 640px) {
  .top-bar__role,
  .top-bar__label {
    display: none;
  }
}
</style>
