/**
 * 测试辅助：信封响应与 fetch 替身。
 *
 * 所有断言都基于真实的请求链路（fetch → 信封解包 → store），
 * 因此这里只做两件事：按接口文档 §1.3 生成响应信封，以及记录每次请求的 URL/头/请求体。
 */
import { vi } from 'vitest'

/** 生成 `{success, code, message, userMessage, requestId, data}` 信封对应的 Response 替身。 */
export function envelope(data = {}, { status = 200, code = 0, message = '', userMessage = '', requestId = 'req-1' } = {}) {
  const success = code === 0 && status >= 200 && status < 300
  return {
    ok: success,
    status,
    json: async () => ({
      success,
      code,
      message: success ? '' : message || 'failure',
      userMessage,
      requestId,
      data: success ? data : null
    })
  }
}

/** 成功信封。 */
export function okResponse(data = {}) {
  return envelope(data)
}

/** 失败信封：status 与 code 必须同时给定，便于覆盖 401/403/409 等分支。 */
export function failResponse({ status = 400, code = 1, userMessage = '操作失败', message = 'failure', requestId = 'req-err' } = {}) {
  return envelope(null, { status, code, message, userMessage, requestId })
}

/**
 * 安装 fetch 替身。
 * @param {Array|Response|Function} responses 依次消费的响应；只剩最后一项时会被重复使用，
 *        因此“写操作 + 之后的重载”可以只写两条。函数形式的响应会收到 (url, options)。
 */
export function installFetch(responses) {
  const calls = []
  const queue = Array.isArray(responses) ? [...responses] : [responses]

  const fetchMock = vi.fn(async (url, options = {}) => {
    calls.push({ url: String(url), options })
    const next = queue.length > 1 ? queue.shift() : queue[0]
    const resolved = typeof next === 'function' ? next(String(url), options) : next
    if (resolved instanceof Error) throw resolved
    return resolved
  })

  vi.stubGlobal('fetch', fetchMock)

  return {
    fetchMock,
    calls,
    count: () => calls.length,
    urlOf: index => calls[index]?.url || '',
    headersOf: index => calls[index]?.options?.headers || {},
    methodOf: index => calls[index]?.options?.method || '',
    bodyOf: index => (calls[index]?.options?.body ? JSON.parse(calls[index].options.body) : null),
    queryOf: index => new URL(calls[index]?.url || '/', 'http://localhost').searchParams,
    /** 找出某个方法 + 路径片段的调用序号。 */
    indexOf: (method, fragment) =>
      calls.findIndex(call => call.options.method === method && String(call.url).includes(fragment)),
    /** 清空“预置队列”，改为始终返回给定的响应（用于断言重载次数）。 */
    always: response => {
      queue.length = 0
      queue.push(response)
    }
  }
}

/** 等待若干个微任务，让 store 里的 await 链推进到可断言的位置。 */
export async function flush(times = 4) {
  for (let index = 0; index < times; index += 1) await Promise.resolve()
}

/** 真实计时器下推进事件循环（用于等待路由跳转、组件挂载等宏任务）。 */
export async function settle(times = 3) {
  for (let index = 0; index < times; index += 1) {
    await new Promise(resolve => setTimeout(resolve, 0))
  }
}

/** 清空 sessionStorage / localStorage，避免用例间串状态。 */
export function clearWebStorage() {
  sessionStorage.clear()
  localStorage.clear()
}
