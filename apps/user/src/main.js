import { createApp } from 'vue'
import { createPinia } from 'pinia'
import App from './App.vue'
import router from './router'
import { vReveal } from './directives/reveal'
import './style.css'
import '../../shared/styles/ice-theme.css'
import './styles/ice-layout.css'
import './styles/motion.css'

// 必须先安装 Pinia：路由守卫会读取 auth store，安装 Pinia 后 useStore() 才能拿到活动实例。
const app = createApp(App)
app.use(createPinia())
app.use(router)
// 全局注册滚动进入指令：任何视图都可以直接用 v-reveal，无需逐页引入。
app.directive('reveal', vReveal)
app.mount('#app')
