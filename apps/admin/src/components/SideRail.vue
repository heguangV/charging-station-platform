<script>
/**
 * 管理端导航目的地：全站唯一一份清单，宽屏侧栏与窄屏抽屉共用同一份定义，
 * 不存在第二套页面或移动端专用路由。
 */
export const NAV_ITEMS = [
  { key: 'overview', to: '/', label: '总览', icon: 'overview' },
  { key: 'stations', to: '/stations', label: '站点', icon: 'station' },
  { key: 'chargers', to: '/chargers', label: '充电桩', icon: 'charger' },
  { key: 'users', to: '/users', label: '用户', icon: 'user' },
  { key: 'flows', to: '/flows', label: '活动流程', icon: 'flow' },
  { key: 'predictions', to: '/predictions', label: '智能预测', icon: 'prediction' },
  { key: 'accounts', to: '/accounts', label: '管理员', icon: 'account' },
  { key: 'ops', to: '/ops', label: '运维', icon: 'ops' }
]

export default { name: 'SideRail' }
</script>

<script setup>
import NavIcon from './NavIcon.vue'

/**
 * 左侧导航栏：宽屏常驻可折叠，窄屏由顶栏按钮拉出为抽屉（同一份 DOM）。
 * 选中态用 ::before 的 scaleY 指示条呈现，切换时是“长出来”而不是硬切换。
 */
const props = defineProps({
  /** 窄屏抽屉是否展开（宽屏下该状态被 CSS 忽略）。 */
  open: { type: Boolean, default: false },
  collapsed: { type: Boolean, default: false },
  activePath: { type: String, default: '/' }
})

defineEmits(['navigate', 'close', 'toggle-collapse'])

/** 首页只匹配 '/'，其余按前缀匹配，保证子路径也高亮。 */
function isActive(item) {
  return item.to === '/' ? props.activePath === '/' : props.activePath.startsWith(item.to)
}
</script>

<template>
  <aside
    class="side-rail"
    :class="{ 'is-open': open, 'is-collapsed': collapsed }"
    data-testid="side-rail"
    aria-label="管理端导航"
  >
    <div class="side-rail__brand">
      <span class="side-rail__logo" aria-hidden="true"><NavIcon name="charger" :size="20" /></span>
      <div class="side-rail__brand-text">
        <strong>NCS 运营中心</strong>
        <span class="side-rail__subtitle">管理端</span>
      </div>
      <button
        type="button"
        class="side-rail__close rail-toggle"
        aria-label="关闭导航"
        @click="$emit('close')"
      >
        <NavIcon name="close" :size="18" />
      </button>
    </div>

    <nav class="side-rail__nav">
      <RouterLink
        v-for="item in NAV_ITEMS"
        :key="item.key"
        class="rail-item"
        :class="{ 'is-active': isActive(item) }"
        :to="item.to"
        :title="item.label"
        :data-testid="`rail-${item.key}`"
        @click="$emit('navigate')"
      >
        <span class="rail-item__icon"><NavIcon :name="item.icon" :size="19" /></span>
        <span class="rail-item__label">{{ item.label }}</span>
      </RouterLink>
    </nav>

    <div class="side-rail__footer">
      <button
        type="button"
        class="btn btn--sm side-rail__collapse"
        :aria-expanded="collapsed ? 'false' : 'true'"
        data-testid="rail-collapse"
        @click="$emit('toggle-collapse')"
      >
        <NavIcon name="menu" :size="16" />
        <span class="rail-item__label">{{ collapsed ? '展开侧栏' : '收起侧栏' }}</span>
      </button>
    </div>
  </aside>
</template>

<style scoped>
.side-rail__brand {
  display: flex;
  align-items: center;
  gap: 10px;
  min-width: 0;
}

.side-rail__logo {
  display: grid;
  place-items: center;
  width: 38px;
  height: 38px;
  flex: none;
  border-radius: var(--ncs-r-sm);
  color: #fff;
  background: linear-gradient(140deg, var(--ncs-brand-bright), var(--ncs-brand-strong));
  box-shadow: 0 6px 16px rgba(13, 122, 111, 0.28);
}

.side-rail__brand-text {
  min-width: 0;
}

.side-rail__brand-text strong {
  display: block;
  font-size: var(--ncs-fs-base);
  letter-spacing: -0.01em;
  white-space: nowrap;
}

.side-rail__subtitle {
  display: block;
  font-size: var(--ncs-fs-xs);
  color: var(--ncs-muted);
  letter-spacing: 0.16em;
}

.side-rail__close {
  display: inline-flex;
  margin-left: auto;
  padding: 6px;
  border: 1px solid var(--ncs-line);
  border-radius: var(--ncs-r-sm);
  background: var(--ncs-surface);
  color: var(--ncs-text-2);
  cursor: pointer;
}

.side-rail__nav {
  display: flex;
  flex-direction: column;
  gap: 3px;
  overflow-y: auto;
}

.rail-item {
  position: relative;
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 9px 12px;
  border-radius: var(--ncs-r-sm);
  color: var(--ncs-text-2);
  text-decoration: none;
  font-size: var(--ncs-fs-sm);
  white-space: nowrap;
  transition:
    background var(--ncs-dur-2) var(--ncs-ease-out),
    color var(--ncs-dur-2) var(--ncs-ease-out),
    transform var(--ncs-dur-1) var(--ncs-ease-out);
}

/* 左侧强调条：选中时从中间展开（scaleY），不是硬切换 */
.rail-item::before {
  content: '';
  position: absolute;
  left: 0;
  top: 50%;
  width: 3px;
  height: 18px;
  border-radius: 999px;
  background: linear-gradient(180deg, var(--ncs-brand-bright), var(--ncs-brand-strong));
  transform: translateY(-50%) scaleY(0);
  transition: transform var(--ncs-dur-2) var(--ncs-ease-spring);
}

.rail-item:hover {
  background: var(--ncs-surface-3);
  color: var(--ncs-text);
}

.rail-item:active {
  transform: scale(0.99);
}

.rail-item.is-active {
  background: var(--ncs-brand-soft);
  color: var(--ncs-brand-strong);
  font-weight: 650;
  box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.6);
}

.rail-item.is-active::before {
  transform: translateY(-50%) scaleY(1);
}

.rail-item__icon {
  display: inline-flex;
  flex: none;
}

.side-rail__footer {
  margin-top: auto;
  padding-top: var(--ncs-s-3);
  border-top: 1px solid var(--ncs-line);
}

.side-rail__collapse {
  width: 100%;
}

/* 折叠态：只留图标，文案隐藏（宽屏生效，窄屏抽屉始终展开） */
@media (min-width: 1024px) {
  .side-rail.is-collapsed .rail-item__label,
  .side-rail.is-collapsed .side-rail__brand-text,
  .side-rail.is-collapsed .side-rail__subtitle {
    display: none;
  }

  .side-rail.is-collapsed .rail-item,
  .side-rail.is-collapsed .side-rail__brand,
  .side-rail.is-collapsed .side-rail__collapse {
    justify-content: center;
    padding-left: 0;
    padding-right: 0;
  }

  .side-rail.is-collapsed .side-rail__collapse .nav-icon {
    margin: 0;
  }
}
</style>
