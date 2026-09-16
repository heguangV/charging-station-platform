/**
 * 统一的 REST 访问层：只与 /api/v1/* 通信，不直接访问任何数据库。
 * 负责统一信封解包、Bearer 令牌、X-Request-ID、业务写入的 Idempotency-Key 与超时。
 */

export const API_BASE = '/api/v1'
/** 普通请求超时（接口文档 §1.9：普通请求 10 秒，前端取 8 秒快速失败）。 */
export const DEFAULT_TIMEOUT_MS = 8000
/** AI 助手依赖 LLM 与地图聚合，调用较慢，单独放宽到 20 秒。 */
export const AGENT_TIMEOUT_MS = 20000

/**
 * 令牌存储策略（仓库安全基线）：令牌只保存在 Pinia 内存状态中，并镜像到 sessionStorage，
 * 绝不写入浏览器长期存储（localStorage 等跨会话存储会长期保留凭据）。
 * 关闭标签页即失效，符合“会话级凭据”的要求。
 */
const TOKEN_STORAGE_KEY = 'ncs.user.accessToken'
const TOKEN_STORAGE_SCOPE = 'session'

export const ApiErrorKind = {
  NETWORK: 'NETWORK',
  TIMEOUT: 'TIMEOUT',
  ABORTED: 'ABORTED',
  INVALID_RESPONSE: 'INVALID_RESPONSE',
  HTTP: 'HTTP'
}

/** 携带 HTTP 状态、稳定业务 code 与可直接展示的 userMessage。 */
export class ApiError extends Error {
  constructor({ status = 0, code = ApiErrorKind.HTTP, message = '', userMessage = '', requestId = '', sessionExpired = false } = {}) {
    super(message || userMessage || '请求失败')
    this.name = 'ApiError'
    this.status = status
    this.code = code
    this.userMessage = userMessage || message || '请求失败，请稍后重试'
    this.requestId = requestId
    this.sessionExpired = sessionExpired
  }
}

function storage() {
  try {
    return typeof sessionStorage === 'undefined' ? null : sessionStorage
  } catch {
    return null
  }
}

function readSessionToken() {
  try {
    return storage()?.getItem(TOKEN_STORAGE_KEY) || null
  } catch {
    return null
  }
}

let accessToken = readSessionToken()
let sessionExpiredHandler = null

export function getAccessToken() {
  return accessToken
}

/** auth store 是令牌的唯一拥有者；这里同步内存与 sessionStorage 镜像。 */
export function setAccessToken(token) {
  accessToken = token || null
  const store = storage()
  if (!store) return
  try {
    if (accessToken) store.setItem(TOKEN_STORAGE_KEY, accessToken)
    else store.removeItem(TOKEN_STORAGE_KEY)
  } catch {
    /* 私密模式下 sessionStorage 可能不可写，内存令牌仍然有效。 */
  }
}

export function clearAccessToken() {
  setAccessToken(null)
}

export function getTokenStorageScope() {
  return TOKEN_STORAGE_SCOPE
}

/** 401/403 视为会话失效，由 auth store 注册回调清理用户状态。 */
export function setSessionExpiredHandler(handler) {
  sessionExpiredHandler = typeof handler === 'function' ? handler : null
}

function notifySessionExpired(error) {
  accessToken = null
  const store = storage()
  try {
    store?.removeItem(TOKEN_STORAGE_KEY)
  } catch {
    /* 忽略存储异常，内存令牌已经清空。 */
  }
  if (sessionExpiredHandler) sessionExpiredHandler(error)
}

/** 统一生成 UUID；环境缺少 crypto.randomUUID 时退化为 v4 形状的随机串。 */
export function randomId() {
  const cryptoObject = globalThis.crypto
  if (cryptoObject && typeof cryptoObject.randomUUID === 'function') return cryptoObject.randomUUID()
  const bytes = new Uint8Array(16)
  if (cryptoObject && typeof cryptoObject.getRandomValues === 'function') cryptoObject.getRandomValues(bytes)
  else for (let index = 0; index < bytes.length; index += 1) bytes[index] = Math.floor(Math.random() * 256)
  bytes[6] = (bytes[6] & 0x0f) | 0x40
  bytes[8] = (bytes[8] & 0x3f) | 0x80
  const hex = Array.from(bytes, value => value.toString(16).padStart(2, '0')).join('')
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`
}

/** 过滤空值，未提供过滤参数表示不过滤（接口文档 §1.5）。 */
export function buildQuery(params = {}) {
  const search = new URLSearchParams()
  for (const [key, value] of Object.entries(params)) {
    if (value === undefined || value === null || value === '') continue
    search.append(key, String(value))
  }
  const query = search.toString()
  return query ? `?${query}` : ''
}

function buildSignal(milliseconds, external) {
  // 优先使用标准 AbortSignal.timeout；旧环境退化为 AbortController + 定时器。
  if (!external && typeof AbortSignal !== 'undefined' && typeof AbortSignal.timeout === 'function') {
    const signal = AbortSignal.timeout(milliseconds)
    return {
      signal,
      timedOut: () => signal.aborted && signal.reason?.name === 'TimeoutError',
      externallyAborted: () => false,
      cleanup: () => {}
    }
  }
  const controller = new AbortController()
  let timedOut = false
  let externallyAborted = Boolean(external?.aborted)
  const timer = setTimeout(() => {
    if (controller.signal.aborted) return
    timedOut = true
    controller.abort(typeof DOMException === 'function' ? new DOMException('请求超时', 'TimeoutError') : new Error('timeout'))
  }, milliseconds)
  const forward = () => {
    if (controller.signal.aborted) return
    externallyAborted = true
    controller.abort(external?.reason)
  }
  if (external) {
    if (external.aborted) forward()
    else external.addEventListener('abort', forward, { once: true })
  }
  return {
    signal: controller.signal,
    timedOut: () => timedOut,
    externallyAborted: () => externallyAborted,
    cleanup: () => {
      clearTimeout(timer)
      external?.removeEventListener('abort', forward)
    }
  }
}

function transportError(error, requestId, { timedOut = false, externallyAborted = false } = {}) {
  const name = error?.name
  if (timedOut || name === 'TimeoutError') {
    return new ApiError({ code: ApiErrorKind.TIMEOUT, message: 'timeout', userMessage: '请求超时，请检查网络后重试', requestId })
  }
  if (externallyAborted || name === 'AbortError') {
    return new ApiError({ code: ApiErrorKind.ABORTED, message: 'aborted', userMessage: '请求已取消', requestId })
  }
  return new ApiError({ code: ApiErrorKind.NETWORK, message: 'network', userMessage: '网络不可用，请检查网络连接后重试', requestId })
}

function statusUserMessage(status) {
  if (status === 401) return '登录已失效，请重新登录'
  if (status === 403) return '无权访问该资源'
  if (status === 404) return '请求的资源不存在'
  if (status === 422) return '提交内容不符合要求，请检查后重试'
  if (status === 429) return '操作过于频繁，请稍后再试'
  if (status === 503 || status === 504) return '服务暂时不可用，请稍后重试'
  return '服务返回异常，请稍后重试'
}

/**
 * 发送请求并解包 `{success, code, message, userMessage, requestId, data}` 信封。
 * @param {string} path 以 `/user/...` 开头的接口路径。
 * @param {object} options method/body/timeout/idempotent/idempotencyKey/signal/headers。
 */
export async function request(path, options = {}) {
  const {
    method = 'GET',
    body,
    timeout = DEFAULT_TIMEOUT_MS,
    idempotent = false,
    idempotencyKey,
    signal: externalSignal,
    headers: extraHeaders
  } = options

  const requestId = randomId()
  const headers = { Accept: 'application/json', 'X-Request-ID': requestId, ...extraHeaders }
  const token = getAccessToken()
  if (token) headers.Authorization = `Bearer ${token}`

  let payload
  if (typeof FormData !== 'undefined' && body instanceof FormData) {
    payload = body
  } else if (body !== undefined && body !== null) {
    headers['Content-Type'] = 'application/json; charset=utf-8'
    payload = JSON.stringify(body)
  }

  // 产生业务写入的 POST 必须携带 Idempotency-Key；查询、登录、AI 只读会话不带（§1.2、§1.8）。
  if (idempotent) headers['Idempotency-Key'] = idempotencyKey || randomId()

  const timeoutControl = buildSignal(timeout, externalSignal)
  let response
  try {
    response = await fetch(`${API_BASE}${path}`, {
      method,
      headers,
      body: payload,
      signal: timeoutControl.signal,
      cache: 'no-store'
    })
  } catch (error) {
    timeoutControl.cleanup()
    throw transportError(error, requestId, {
      timedOut: timeoutControl.timedOut(),
      externallyAborted: timeoutControl.externallyAborted()
    })
  }

  let envelope = null
  try {
    envelope = await response.json()
  } catch (error) {
    if (timeoutControl.signal.aborted || error?.name === 'AbortError' || error?.name === 'TimeoutError') {
      throw transportError(error, requestId, {
        timedOut: timeoutControl.timedOut(),
        externallyAborted: timeoutControl.externallyAborted()
      })
    }
    envelope = null
  } finally {
    timeoutControl.cleanup()
  }
  if (!envelope || typeof envelope !== 'object') {
    const sessionExpired = response.status === 401 || response.status === 403
    const apiError = new ApiError({
      status: response.status,
      code: ApiErrorKind.INVALID_RESPONSE,
      message: 'invalid envelope',
      userMessage: statusUserMessage(response.status),
      requestId,
      sessionExpired
    })
    if (sessionExpired) notifySessionExpired(apiError)
    throw apiError
  }

  if (envelope.success !== true || envelope.code !== 0) {
    const code = typeof envelope.code === 'number' ? envelope.code : ApiErrorKind.HTTP
    const apiError = new ApiError({
      status: response.status,
      // userMessage 缺失时回退到 message；两者都受接口文档约束（不含 SQL、路径、堆栈、令牌）。
      code,
      message: typeof envelope.message === 'string' ? envelope.message : '',
      userMessage:
        (typeof envelope.userMessage === 'string' && envelope.userMessage) ||
        (typeof envelope.message === 'string' && envelope.message) ||
        statusUserMessage(response.status),
      requestId:
        (typeof envelope.requestId === 'string' && envelope.requestId) ||
        // Go 契约的错误信封把请求 ID 放在 traceId 字段。
        (typeof envelope.traceId === 'string' && envelope.traceId) ||
        requestId,
      sessionExpired: response.status === 401 || response.status === 403 || code === 401 || code === 403
    })
    if (apiError.sessionExpired) notifySessionExpired(apiError)
    throw apiError
  }

  return envelope.data === undefined || envelope.data === null ? {} : envelope.data
}

export const api = {
  get: (path, params, options = {}) => request(`${path}${buildQuery(params)}`, { ...options, method: 'GET' }),
  post: (path, body, options = {}) => request(path, { ...options, method: 'POST', body }),
  put: (path, body, options = {}) => request(path, { ...options, method: 'PUT', body }),
  delete: (path, body, options = {}) => request(path, { ...options, method: 'DELETE', body })
}
