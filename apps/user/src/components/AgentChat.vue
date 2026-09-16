<script setup>
import { computed, nextTick, ref, watch } from 'vue'
import { storeToRefs } from 'pinia'
import AgentResult from './AgentResult.vue'
import { useAgentStore } from '@/stores/agent'
import { useStationStore } from '@/stores/station'

/**
 * AI 助手聊天面板：空提示词不发送、等待中有加载态、失败可重试，
 * 结构化结果交给 AgentResult 渲染。所有 LLM 与地图调用都经服务端 POST /agent/chat。
 */
const EXAMPLES = ['帮我找一个附近有快充并且旁边能吃饭的充电站', '充电站附近有什么咖啡店', '导航到最近的充电站']

const agent = useAgentStore()
const station = useStationStore()
const { messages, loading, error } = storeToRefs(agent)

const input = ref('')
const messageList = ref(null)

const canSend = computed(() => input.value.trim().length > 0 && !loading.value)
const quickPrompts = computed(() => EXAMPLES)

function agentLocation() {
  if (!station.hasLocation) return null
  return {
    latitudeE6: station.location.latitudeE6,
    longitudeE6: station.location.longitudeE6,
    coordinateType: station.location.coordinateType || 'wgs84'
  }
}

async function scrollToLatest() {
  await nextTick()
  if (messageList.value) messageList.value.scrollTop = messageList.value.scrollHeight
}

async function submit() {
  const text = input.value.trim()
  // 空提示词不发送，避免无意义的服务端调用。
  if (!text || loading.value) return
  input.value = ''
  await agent.send(text, {
    location: agentLocation(),
    chargerType: station.chargerType === 0 || station.chargerType === 1 ? station.chargerType : undefined
  })
  await scrollToLatest()
}

async function useExample(prompt) {
  input.value = prompt
  await submit()
}

async function retry() {
  await agent.retry()
  await scrollToLatest()
}

watch(
  () => messages.value.length,
  () => {
    void scrollToLatest()
  }
)
</script>

<template>
  <section class="agent-chat" data-testid="agent-chat">
    <div class="agent-chat__examples">
      <span class="muted">试试：</span>
      <button
        v-for="(prompt, index) in quickPrompts"
        :key="prompt"
        type="button"
        class="chip"
        :data-testid="`agent-example-${index}`"
        :disabled="loading"
        @click="useExample(prompt)"
      >
        {{ prompt }}
      </button>
    </div>

    <div ref="messageList" class="agent-chat__messages" data-testid="agent-messages" aria-live="polite">
      <p v-if="!messages.length" class="muted" data-testid="agent-empty-hint">
        可以问我附近的充电站、站点周边的餐厅咖啡店，或直接让我规划到最近充电站的路线。
      </p>

      <TransitionGroup name="list">
        <article
          v-for="message in messages"
          :key="message.id"
          class="agent-chat__bubble"
          :class="`agent-chat__bubble--${message.role}`"
          :data-testid="`agent-message-${message.role}`"
        >
          <p class="agent-chat__text">{{ message.content }}</p>
          <AgentResult v-if="message.result" :result="message.result" />
        </article>
      </TransitionGroup>

      <p v-if="loading" class="agent-chat__loading overlay-rise" data-testid="agent-loading" role="status">
        <span class="typing-dot" aria-hidden="true"></span>
        <span class="typing-dot" aria-hidden="true"></span>
        <span class="typing-dot" aria-hidden="true"></span>
        <span>正在为你查询，请稍候…</span>
      </p>
    </div>

    <div v-if="error" class="alert alert--error" data-testid="agent-error" role="alert">
      <p data-testid="agent-error-message">{{ error }}</p>
      <button type="button" class="btn btn--primary" data-testid="agent-retry" :disabled="loading" @click="retry">
        重试
      </button>
    </div>

    <form class="agent-chat__composer" data-testid="agent-composer" @submit.prevent="submit">
      <label class="sr-only" for="agent-input">输入你的问题</label>
      <textarea
        id="agent-input"
        v-model="input"
        data-testid="agent-input"
        rows="2"
        maxlength="600"
        placeholder="例如：帮我找一个附近有快充并且旁边能吃饭的充电站"
        @keydown.enter.exact.prevent="submit"
      ></textarea>
      <button type="submit" class="btn btn--primary" data-testid="agent-send" :disabled="!canSend">发送</button>
    </form>
  </section>
</template>

<style scoped>
.agent-chat {
  display: flex;
  flex-direction: column;
  gap: var(--ncs-s-3);
  padding: var(--ncs-s-4);
  background: var(--ncs-surface);
  border: 1px solid var(--ncs-line);
  border-radius: var(--ncs-r-lg);
  box-shadow: var(--ncs-shadow-1);
}

.agent-chat__examples {
  display: flex;
  flex-wrap: wrap;
  gap: var(--ncs-s-2);
  align-items: center;
}

.agent-chat__messages {
  display: flex;
  flex-direction: column;
  gap: var(--ncs-s-3);
  max-height: 58vh;
  overflow-y: auto;
  padding: var(--ncs-s-2);
  scroll-behavior: smooth;
  position: relative;
}

.agent-chat__bubble {
  position: relative;
  border-radius: var(--ncs-r-md);
  padding: 12px 14px;
  max-width: 100%;
  transition:
    opacity var(--ncs-dur-2) var(--ncs-ease-out),
    transform var(--ncs-dur-2) var(--ncs-ease-out);
}

.agent-chat__bubble--user {
  align-self: flex-end;
  max-width: 88%;
  background: linear-gradient(140deg, rgba(13, 122, 111, 0.14), rgba(20, 161, 146, 0.1));
  border: 1px solid rgba(13, 122, 111, 0.16);
}

.agent-chat__bubble--assistant {
  background: var(--ncs-surface-2);
  border: 1px solid var(--ncs-line);
}

.agent-chat__text {
  margin: 0 0 var(--ncs-s-2);
  white-space: pre-wrap;
  line-height: 1.62;
}

.agent-chat__text:last-child {
  margin-bottom: 0;
}

.agent-chat__loading {
  display: flex;
  align-items: center;
  gap: 2px;
  margin: 0;
  padding: 10px 14px;
  align-self: flex-start;
  border-radius: var(--ncs-r-pill);
  background: var(--ncs-surface-2);
  border: 1px solid var(--ncs-line);
  color: var(--ncs-muted);
  font-size: var(--ncs-fs-sm);
}

.agent-chat__composer {
  display: flex;
  gap: var(--ncs-s-2);
  align-items: flex-end;
  padding-top: var(--ncs-s-3);
  border-top: 1px solid var(--ncs-line);
}

@media (max-width: 900px) {
  .agent-chat__messages {
    max-height: none;
  }

  .agent-chat {
    border-radius: var(--ncs-r-md);
  }
}
</style>
