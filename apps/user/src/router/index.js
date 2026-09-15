import { createRouter, createWebHistory } from 'vue-router'
import AppShell from '@/components/AppShell.vue'

/**
 * 唯一页面集合：PC 与移动端共用同一批路由，只靠响应式布局切换呈现方式。
 * 未匹配的路径统一回落到首页。
 */
const routes = [
  {
    path: '/',
    component: AppShell,
    children: [
      { path: '', name: 'home', component: () => import('@/views/HomeView.vue'), meta: { title: '附近充电站' } },
      { path: 'stations/:stationId', name: 'station', component: () => import('@/views/StationView.vue'), meta: { title: '站点详情' } },
      { path: 'charging', name: 'charging', component: () => import('@/views/ChargingView.vue'), meta: { title: '充电流程' } },
      { path: 'agent', name: 'agent', component: () => import('@/views/AgentView.vue'), meta: { title: 'AI 助手' } },
      { path: 'orders', name: 'orders', component: () => import('@/views/OrdersView.vue'), meta: { title: '我的订单' } },
      { path: 'profile', name: 'profile', component: () => import('@/views/ProfileView.vue'), meta: { title: '我的' } }
    ]
  },
  { path: '/:pathMatch(.*)*', redirect: '/' }
]

const router = createRouter({
  history: createWebHistory(),
  routes,
  scrollBehavior: () => ({ top: 0 })
})

export default router
