<script setup>
/**
 * 通用表单弹窗：站点新增/编辑、设备批量创建、管理员创建、价格版本与预测范围等都复用同一套字段渲染。
 *
 * 字段定义：[{ key, label, type: text|number|password|select|textarea|datetime|checkbox,
 *             options, placeholder, required, min, max, step, help, default, suffix }]
 * 本地只做必填与范围校验（快速失败），业务规则仍由服务端校验。
 */
import { computed, ref, watch } from 'vue'

const props = defineProps({
  title: { type: String, required: true },
  hint: { type: String, default: '' },
  fields: { type: Array, required: true },
  submitLabel: { type: String, default: '提交' },
  loading: { type: Boolean, default: false },
  error: { type: String, default: '' },
  testId: { type: String, required: true },
  wide: { type: Boolean, default: true }
})

const emit = defineEmits(['submit', 'cancel'])

const values = ref({})
const localError = ref('')

function blankValue(field) {
  if (field.default !== undefined) return field.default
  if (field.type === 'checkbox') return false
  if (field.type === 'select' && Array.isArray(field.options) && field.options.length > 0) {
    return field.options[0].value
  }
  return ''
}

function reset() {
  const next = {}
  for (const field of props.fields) next[field.key] = blankValue(field)
  values.value = next
  localError.value = ''
}

// 打开新表单（标题或字段集合变化）时重置，避免上一次的输入串场。
watch(() => [props.title, props.fields], reset, { immediate: true, deep: false })

const message = computed(() => localError.value || props.error)

/** 校验并归一化：数字字段转整数，其余保持字符串。 */
function normalize() {
  const payload = {}
  for (const field of props.fields) {
    const raw = values.value[field.key]
    if (field.type === 'checkbox') {
      payload[field.key] = raw === true
      continue
    }
    const text = typeof raw === 'string' ? raw.trim() : raw === undefined || raw === null ? '' : String(raw)
    if (field.required && text === '') {
      localError.value = `请填写${field.label}`
      return null
    }
    if (text === '') {
      if (field.type === 'number') payload[field.key] = null
      else payload[field.key] = ''
      continue
    }
    if (field.type === 'number') {
      const parsed = Number(text)
      if (!Number.isFinite(parsed)) {
        localError.value = `${field.label}必须是数字`
        return null
      }
      const allowDecimal = field.integer === false
      if (!allowDecimal && !Number.isInteger(parsed)) {
        localError.value = `${field.label}必须是整数`
        return null
      }
      if (field.min !== undefined && parsed < field.min) {
        localError.value = `${field.label}不能小于 ${field.min}`
        return null
      }
      if (field.max !== undefined && parsed > field.max) {
        localError.value = `${field.label}不能大于 ${field.max}`
        return null
      }
      payload[field.key] = allowDecimal ? parsed : Math.trunc(parsed)
      continue
    }
    if (field.type === 'datetime') {
      payload[field.key] = text
      continue
    }
    if (field.minLength !== undefined && text.length < field.minLength) {
      localError.value = `${field.label}至少 ${field.minLength} 个字符`
      return null
    }
    if (field.maxLength !== undefined && text.length > field.maxLength) {
      localError.value = `${field.label}不能超过 ${field.maxLength} 个字符`
      return null
    }
    payload[field.key] = text
  }
  localError.value = ''
  return payload
}

function submit() {
  const payload = normalize()
  if (!payload) return
  emit('submit', payload)
}

function cancel() {
  localError.value = ''
  emit('cancel')
}

/**
 * 回车即提交；文本域内保留换行。
 * 提交按钮用 type="button" + 点击：jsdom 不实现原生表单提交，
 * 且可避免浏览器里回车触发“默认按钮点击 + 表单提交”的双重写入。
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
        <label v-for="field in fields" :key="field.key" class="field">
          <span>{{ field.label }}</span>

          <select
            v-if="field.type === 'select'"
            v-model="values[field.key]"
            :data-testid="`${testId}-${field.key}`"
          >
            <option v-for="option in field.options" :key="String(option.value)" :value="option.value">
              {{ option.label }}
            </option>
          </select>

          <span v-else-if="field.type === 'checkbox'" class="checkbox-row">
            <input
              v-model="values[field.key]"
              type="checkbox"
              :data-testid="`${testId}-${field.key}`"
            />
            <span class="muted">{{ field.help || field.label }}</span>
          </span>

          <textarea
            v-else-if="field.type === 'textarea'"
            v-model="values[field.key]"
            rows="3"
            :placeholder="field.placeholder || ''"
            :data-testid="`${testId}-${field.key}`"
          ></textarea>

          <input
            v-else
            v-model="values[field.key]"
            :type="field.type === 'number' ? 'number' : field.type === 'password' ? 'password' : field.type === 'datetime' ? 'datetime-local' : 'text'"
            :placeholder="field.placeholder || ''"
            :data-testid="`${testId}-${field.key}`"
          />

          <span v-if="field.help && field.type !== 'checkbox'" class="field__help">{{ field.help }}</span>
        </label>
      </div>

      <p v-if="message" class="alert alert--error dialog__error" :data-testid="`${testId}-error`">
        {{ message }}
      </p>

      <div class="dialog__footer">
        <button type="button" class="btn" :data-testid="`${testId}-cancel`" @click="cancel">取消</button>
        <button
          type="button"
          class="btn btn--primary"
          :disabled="loading"
          :data-testid="`${testId}-submit`"
          @click="submit"
        >
          {{ loading ? '提交中…' : submitLabel }}
        </button>
      </div>
    </form>
  </div>
</template>

<style scoped>
.checkbox-row {
  display: flex;
  align-items: center;
  gap: var(--ncs-s-2);
}

.checkbox-row input {
  width: auto;
}

.field__help {
  font-size: var(--ncs-fs-xs);
  color: var(--ncs-muted);
}
</style>
