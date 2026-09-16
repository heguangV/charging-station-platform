<script setup>
/**
 * 二次确认弹窗。
 *
 * 管理端的敏感操作都要求“确认 + 原因”（强制释放还要求目标设备状态），
 * 因此这里把确认框、原因校验与附加字段统一实现，页面只负责传字段定义与处理提交结果。
 * 校验只在本地做长度/必填判断，业务约束仍以服务端为准。
 */
import { computed, ref, watch } from 'vue'

const props = defineProps({
  title: { type: String, required: true },
  hint: { type: String, default: '' },
  confirmLabel: { type: String, default: '确认操作' },
  cancelLabel: { type: String, default: '取消' },
  danger: { type: Boolean, default: false },
  /** 原因输入：管理端所有敏感写操作都要求 2~200 个可见字符。 */
  requireReason: { type: Boolean, default: true },
  reasonLabel: { type: String, default: '操作原因' },
  reasonPlaceholder: { type: String, default: '填写操作原因，至少 2 个字' },
  minReasonLength: { type: Number, default: 2 },
  maxReasonLength: { type: Number, default: 200 },
  /** 附加字段：[{ key, label, type, options, default, help }] */
  fields: { type: Array, default: () => [] },
  loading: { type: Boolean, default: false },
  error: { type: String, default: '' },
  /**
   * 弹窗容器 testid（默认即约定值 confirm-dialog）；
   * 内部固定使用 confirm-reason / confirm-error / confirm-cancel / confirm-submit，
   * 附加字段使用 confirm-field-<key>，命名不随调用方变化。
   */
  testId: { type: String, default: 'confirm-dialog' },
  wide: { type: Boolean, default: false }
})

const emit = defineEmits(['confirm', 'cancel'])

/** 初始值：原因留空，其余字段取 default（或选项首项）。 */
function initialValues() {
  const values = { reason: '' }
  for (const field of props.fields) {
    if (field.default !== undefined) values[field.key] = field.default
    else if (field.type === 'select' && Array.isArray(field.options) && field.options.length > 0) {
      values[field.key] = field.options[0].value
    } else if (field.type === 'checkbox') values[field.key] = false
    else values[field.key] = ''
  }
  return values
}

const values = ref(initialValues())
const localError = ref('')

// 每次打开新弹窗（标题或字段变化）时重置表单，避免残留上一次的原因。
watch(
  () => [props.title, props.fields],
  () => {
    values.value = initialValues()
    localError.value = ''
  }
)

const message = computed(() => localError.value || props.error)

function submit() {
  const reason = String(values.value.reason || '').trim()
  if (props.requireReason) {
    if (reason.length < props.minReasonLength) {
      localError.value = `请填写操作原因（至少 ${props.minReasonLength} 个字）`
      return
    }
    if (reason.length > props.maxReasonLength) {
      localError.value = `操作原因不能超过 ${props.maxReasonLength} 个字`
      return
    }
  }
  localError.value = ''
  emit('confirm', { ...values.value, reason })
}

function cancel() {
  localError.value = ''
  emit('cancel')
}

/**
 * 回车即提交。提交按钮使用 type="button" + 点击处理：
 * jsdom 不实现原生表单提交（点击 submit 按钮不会触发 submit 事件），
 * 同时这样也避免浏览器里“回车触发默认按钮点击 + 表单提交”导致的双重写入。
 */
function handleEnter(event) {
  if (event?.target && String(event.target.tagName).toUpperCase() === 'TEXTAREA') return
  if (event?.preventDefault) event.preventDefault()
  submit()
}
</script>

<template>
  <div class="dialog-backdrop" :data-testid="testId" role="dialog" aria-modal="true">
    <form
      class="dialog overlay-pop"
      :class="{ 'dialog--wide': wide }"
      @submit.prevent="submit"
      @keydown.enter="handleEnter"
    >
      <h2 class="dialog__title">{{ title }}</h2>
      <p v-if="hint" class="dialog__hint">{{ hint }}</p>

      <div class="dialog__body">
        <label v-if="requireReason" class="field">
          <span>{{ reasonLabel }}</span>
          <textarea
            v-model="values.reason"
            rows="3"
            :placeholder="reasonPlaceholder"
            data-testid="confirm-reason"
          ></textarea>
        </label>

        <label v-for="field in fields" :key="field.key" class="field">
          <span>{{ field.label }}</span>
          <select
            v-if="field.type === 'select'"
            v-model="values[field.key]"
            :data-testid="`confirm-field-${field.key}`"
          >
            <option v-for="option in field.options" :key="String(option.value)" :value="option.value">
              {{ option.label }}
            </option>
          </select>
          <input
            v-else-if="field.type === 'checkbox'"
            v-model="values[field.key]"
            type="checkbox"
            :data-testid="`confirm-field-${field.key}`"
          />
          <input
            v-else
            v-model="values[field.key]"
            :type="field.type === 'number' ? 'number' : 'text'"
            :placeholder="field.placeholder || ''"
            :data-testid="`confirm-field-${field.key}`"
          />
          <span v-if="field.help" class="field__help">{{ field.help }}</span>
        </label>
      </div>

      <p v-if="message" class="alert alert--error dialog__error" data-testid="confirm-error">
        {{ message }}
      </p>

      <div class="dialog__footer">
        <button type="button" class="btn" data-testid="confirm-cancel" @click="cancel">
          {{ cancelLabel }}
        </button>
        <button
          type="button"
          class="btn"
          :class="danger ? 'btn--danger' : 'btn--primary'"
          :disabled="loading"
          data-testid="confirm-submit"
          @click="submit"
        >
          {{ loading ? '提交中…' : confirmLabel }}
        </button>
      </div>
    </form>
  </div>
</template>

<style scoped>
.field__help {
  font-size: var(--ncs-fs-xs);
  color: var(--ncs-muted);
}
</style>
