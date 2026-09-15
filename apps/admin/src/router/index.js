import { createRouter, createWebHistory } from 'vue-router'
import AdminShell from '@/components/AdminShell.vue'
import { useAuthStore } from '@/stores/auth'

/**
 * 唯一的页面集合与路由表：宽屏控制台与窄屏浏览器共用同一批路由，
 * 只靠 CSS 断点切换呈现方式。未匹配路径统一回落到总览。
 */
export const routes = [
  {
    path: '/login',
    name: 'login',
    component: () => import('@/views/LoginView.vue'),
    meta: { title: '管理员登录', crumb: 'NCS 充电运营中心', public: true }
  },
  {
    path: '/',
    component: AdminShell,
    children: [
      {
        path: '',
        name: 'overview',
        component: () => import('@/views/OverviewView.vue'),
        meta: { title: '运营总览', crumb: '运营 / 总览' }
      },
      {
        path: 'stations',
        name: 'stations',
        component: () => import('@/views/StationsView.vue'),
        meta: { title: '站点管理', crumb: '运营 / 站点管理' }
      },
      {
        path: 'chargers',
        name: 'chargers',
        component: () => import('@/views/ChargersView.vue'),
        meta: { title: '充电桩管理', crumb: '运营 / 充电桩管理' }
      },
      {
        path: 'users',
        name: 'users',
        component: () => import('@/views/UsersView.vue'),
        meta: { title: '用户管理', crumb: '运营 / 用户管理' }
      },
      {
        path: 'flows',
        name: 'flows',
        component: () => import('@/views/FlowsView.vue'),
        meta: { title: '活动流程', crumb: '运营 / 活动流程' }
      },
      {
        path: 'predictions',
        name: 'predictions',
        component: () => import('@/views/PredictionsView.vue'),
        meta: { title: '智能预测', crumb: '运营 / 智能预测' }
      },
      {
        path: 'accounts',
        name: 'accounts',
        component: () => import('@/views/AccountsView.vue'),
        meta: { title: '管理员账号', crumb: '系统 / 管理员账号' }
      },
      {
        path: 'ops',
        name: 'ops',
        component: () => import('@/views/OpsView.vue'),
        meta: { title: '运维', crumb: '系统 / 运维' }
      }
    ]
  },
  { path: '/:pathMatch(.*)*', redirect: '/' }
]

const router = createRouter({
  history: createWebHistory(),
  routes,
  scrollBehavior: () => ({ top: 0 })
})

/**
 * 未登录一律回到登录页，并带上原目标地址；已登录访问 /login 时回到总览。
 * 导出为具名函数：测试用同一份守卫挂到内存路由上，不复制规则。
 */
export function authGuard(to) {
  const auth = useAuthStore()
  if (to.meta?.public === true) {
    return auth.isLoggedIn ? { path: '/' } : true
  }
  if (!auth.isLoggedIn) {
    return { path: '/login', query: to.fullPath === '/' ? undefined : { redirect: to.fullPath } }
  }
  return true
}

router.beforeEach(authGuard)

export default router
