<script setup>
/**
 * 敏感操作重新验证弹窗（接口文档 §6.2）。
 *
 * 只在服务端返回 REAUTH_REQUIRED(23) 时出现：说明距上次验证已超过 15 分钟。
 * 验证成功后由 auth store 用**同一个幂等键**重试原请求，因此弹窗本身不关心业务内容。
 */
import { computed, ref, watch } from 'vue'

const props = defineProps({
  loading: { type: Boolean, default: false },
  error: { type: String, default: '' },
  hint: {
    type: String,
    default: '该操作属于敏感操作，请输入当前登录密码完成重新验证（验证后 15 分钟内有效）。'
  }
})

const emit = defineEmits(['submit', 'cancel'])

const password = ref('')
const localError = ref('')

// 每次重新打开都清空输入，避免密码残留。
watch(
  () => props.error,
  () => {
    localError.value = ''
  }
)

const message = computed(() => localError.value || props.error)

function submit() {
  if (password.value === '') {
    localError.value = '请输入当前登录密码'
    return
  }
  localError.value = ''
  emit('submit', password.value)
}

function cancel() {
  password.value = ''
  localError.value = ''
  emit('cancel')
}

/** 回车即提交；按钮用 type="button" + 点击，理由与 ConfirmDialog 相同。 */
function handleEnter(event) {
  if (event?.preventDefault) event.preventDefault()
  submit()
}
</script>

<template>
  <div class="dialog-backdrop" data-testid="reauth-dialog" role="dialog" aria-modal="true">
    <form class="dialog overlay-pop" @submit.prevent="submit" @keydown.enter="handleEnter">
      <h2 class="dialog__title">重新验证管理员身份</h2>
      <p class="dialog__hint">{{ hint }}</p>

      <div class="dialog__body">
        <label class="field">
          <span>当前登录密码</span>
          <input
            v-model="password"
            type="password"
            autocomplete="current-password"
            placeholder="请输入当前密码"
            data-testid="reauth-password"
          />
        </label>
      </div>

      <p v-if="message" class="alert alert--error dialog__error" data-testid="reauth-error">
        {{ message }}
      </p>

      <div class="dialog__footer">
        <button type="button" class="btn" data-testid="reauth-cancel" @click="cancel">取消</button>
        <button type="button" class="btn btn--primary" :disabled="loading" data-testid="reauth-submit" @click="submit">
          {{ loading ? '验证中…' : '验证并继续' }}
        </button>
      </div>
    </form>
  </div>
</template>
