<template>
  <div class="login-page">
    <form class="tech-card login-form" @submit.prevent="submit">
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
.login-page { height: 100%; display: grid; place-items: center; }
.login-form { width: 420px; padding: 36px; display: grid; gap: 20px; }
h1 { font-size: 24px; }
p { color: var(--text-secondary); }
label { display: grid; gap: 8px; }
input, button { padding: 12px; border: 1px solid var(--panel-border); border-radius: 4px; font: inherit; }
button { color: var(--text-primary); background: #174b6b; cursor: pointer; }
</style>
