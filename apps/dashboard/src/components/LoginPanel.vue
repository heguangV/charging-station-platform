<template>
  <div class="login-page">
    <section class="welcome-panel">
      <span class="brand-name">NCS · 充电运营</span>
      <h2>每一度电，<br />都清晰可见。</h2>
      <p>连接站点、设备与运营数据</p>
      <img src="../assets/charging-scene.png" alt="电动汽车与充电桩" />
    </section>
    <form class="tech-card login-form" @submit.prevent="submit">
      <div class="brand-mark" aria-hidden="true"><svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M13 2 4 14h7l-1 8 10-13h-7z" /></svg></div>
      <h1>NCS 运营大屏登录</h1>
      <p>运营管理员或决策查看者只读访问</p>
      <label>账号<input v-model="username" autocomplete="username" required maxlength="64" /></label>
      <label>密码<input v-model="password" type="password" autocomplete="current-password" required maxlength="128" /></label>
      <p v-if="store.error" role="alert">{{ store.error }}</p>
      <button type="submit" :disabled="store.isLoading">{{ store.isLoading ? '登录中…' : '登录' }}</button>
    </form>
  </div>
</template>
<script setup lang="ts">
import { ref } from 'vue'
import { useDashboardStore } from '../stores/dashboardStore'
const store = useDashboardStore()
const username = ref('')
const password = ref('')
async function submit() {
  if (store.isLoading) return
  await store.login(username.value, password.value)
  password.value = ''
}
</script>
<style scoped>
.login-page { min-height: 100vh; display: grid; grid-template-columns: 1.2fr 1fr; align-items: center; gap: 8vw; padding: 8vh 8vw; background: #fff; }
.welcome-panel { min-width: 0; }
.brand-name { display: block; font-size: 16px; font-weight: 700; color: #16754f; margin-bottom: 40px; }
h2 { font-size: clamp(36px, 4vw, 64px); line-height: 1.3; letter-spacing: -1px; color: #182c3a; margin-bottom: 20px; }
.welcome-panel img { width: 100%; border-radius: 20px; margin-top: 30px; }
.login-form { width: min(100%, 420px); border: 0; padding: 24px 0; display: grid; gap: 24px; }
h1 { font-size: 28px; line-height: 1.4; color: #192b39; }
p { color: #64717b; font-size: 15px; line-height: 1.7; }
label { display: grid; gap: 10px; font-size: 15px; font-weight: 600; }
input, button { min-width: 0; padding: 16px; border: 1px solid #e2e7ec; border-radius: 12px; }
input { background: #f5f6f8; font-size: 17px; color: #172b3a; }
button { background: #102f46; color: #fff; font-size: 17px; font-weight: 700; }
button:hover { background: #204860; }
.brand-mark { display: none; }
@media (max-width: 850px) { .login-page { grid-template-columns: 1fr; gap: 20px; padding: 32px 24px; } .welcome-panel { display: none; } .login-form { margin: auto; } }

</style>
