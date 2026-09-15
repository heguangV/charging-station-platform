/**
 * 管理端统一 REST 访问层：只与 /api/v1/admin/* 通信，不直接访问任何数据库。
 *
 * 职责与用户端一致：统一信封解包、Bearer 令牌、X-Request-ID、业务写入的 Idempotency-Key
 * 与超时控制；差别在于管理端的敏感操作需要重新验证（REAUTH_REQUIRED），
 * 该错误必须先交给 auth store 走“重新验证后原幂等键重试”，绝不能当作会话失效处理。
 */

export const API_BASE = '/api/v1'
/** 普通请求超时（接口文档 §1.9：普通请求 10 秒，前端取 8 秒快速失败）。 */
export const DEFAULT_TIMEOUT_MS = 8000
/** 统计类请求由服务端聚合，允许更长的等待时间。 */
export const STATS_TIMEOUT_MS = 15000

/** 稳定业务错误码（接口文档 §1.10），代码分支只依赖这些常量。 */
export const ERROR_CODES = {
  OK: 0,
  INVALID_ARGUMENT: 1,
  VALIDATION_FAILED: 2,
  DATABASE_ERROR: 3,
  NOT_FOUND: 4,
  ALREADY_EXISTS: 5,
  USER_FROZEN: 6,
  INSUFFICIENT_BALANCE: 7,
  CHARGER_UNAVAILABLE: 8,
  ACTIVE_FLOW_EXISTS: 9,
  ALLOCATION_CONFLICT: 10,
  TRANSACTION_FAILED: 11,
  EXTERNAL_SERVICE_UNAVAILABLE: 12,
  INTERNAL_ERROR: 13,
  IDEMPOTENCY_CONFLICT: 14,
  INVALID_STATE_TRANSITION: 15,
  QUOTE_EXPIRED: 16,
  RESERVATION_EXPIRED: 17,
  DEBT_OUTSTANDING: 18,
  RATE_LIMITED: 19,
  CODE_INVALID: 20,
  CODE_EXPIRED: 21,
  VERSION_CONFLICT: 22,
  REAUTH_REQUIRED: 23,
  UNAUTHORIZED: 401,
  FORBIDDEN: 403
}

/**
 * 令牌存储策略（仓库安全基线）：令牌只保存在 Pinia 内存状态中并镜像到 sessionStorage，
 * 绝不写入 localStorage 等跨会话长期存储。
 */
export const TOKEN_STORAGE_KEY = 'ncs.admin.accessToken'
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
  constructor({
    status = 0,
    code = ApiErrorKind.HTTP,
    message = '',
    userMessage = '',
    requestId = '',
    sessionExpired = false,
    reauthRequired = false
  } = {}) {
    super(message || userMessage || '请求失败')
    this.name = 'ApiError'
    this.status = status
    this.code = code
    this.userMessage = userMessage || message || '请求失败，请稍后重试'
    this.requestId = requestId
    this.sessionExpired = sessionExpired
    this.reauthRequired = reauthRequired
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

/** 401/403（重新验证除外）视为会话失效，由 auth store 注册回调清理状态。 */
export function setSessionExpiredHandler(handler) {
  sessionExpiredHandler = typeof handler === 'function' ? handler : null
}

function notifySessionExpired(error) {
  accessToken = null
  try {
    storage()?.removeItem(TOKEN_STORAGE_KEY)
  } catch {
    /* 忽略存储异常，内存令牌已清空。 */
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

/** 过滤空值：未提供过滤参数表示不过滤，空字符串按未提供处理（§1.5）。 */
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
    return { signal: AbortSignal.timeout(milliseconds), cleanup: () => {} }
  }
  const controller = new AbortController()
  const timer = setTimeout(() => {
    controller.abort(
      typeof DOMException === 'function' ? new DOMException('请求超时', 'TimeoutError') : new Error('timeout')
    )
  }, milliseconds)
  const forward = () => controller.abort(external?.reason)
  if (external) {
    if (external.aborted) forward()
    else external.addEventListener('abort', forward, { once: true })
  }
  return {
    signal: controller.signal,
    cleanup: () => {
      clearTimeout(timer)
      external?.removeEventListener('abort', forward)
    }
  }
}

function transportError(error, requestId) {
  const name = error?.name
  if (name === 'TimeoutError') {
    return new ApiError({
      code: ApiErrorKind.TIMEOUT,
      message: 'timeout',
      userMessage: '请求超时，请检查网络后重试',
      requestId
    })
  }
  if (name === 'AbortError') {
    return new ApiError({ code: ApiErrorKind.ABORTED, message: 'aborted', userMessage: '请求已取消', requestId })
  }
  return new ApiError({
    code: ApiErrorKind.NETWORK,
    message: 'network',
    userMessage: '网络不可用，请检查网络连接后重试',
    requestId
  })
}

function statusUserMessage(status) {
  if (status === 401) return '登录已失效，请重新登录'
  if (status === 403) return '无权访问该资源'
  if (status === 404) return '请求的资源不存在'
  if (status === 409) return '当前状态不允许该操作'
  if (status === 422) return '提交内容不符合要求，请检查后重试'
  if (status === 429) return '操作过于频繁，请稍后再试'
  if (status === 503 || status === 504) return '服务暂时不可用，请稍后重试'
  return '服务返回异常，请稍后重试'
}

/**
 * 发送请求并解包 `{success, code, message, userMessage, requestId, data}` 信封。
 * @param {string} path 以 `/admin/...` 开头的接口路径。
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

  // 产生业务写入的 POST 必填幂等键（§1.2、§1.8）；GET、查询与登录不带。
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
    throw transportError(error, requestId)
  } finally {
    timeoutControl.cleanup()
  }

  let envelope = null
  try {
    envelope = await response.json()
  } catch {
    envelope = null
  }
  if (!envelope || typeof envelope !== 'object') {
    throw new ApiError({
      status: response.status,
      code: ApiErrorKind.INVALID_RESPONSE,
      message: 'invalid envelope',
      userMessage: statusUserMessage(response.status),
      requestId
    })
  }

  if (envelope.success !== true || envelope.code !== 0) {
    const code = typeof envelope.code === 'number' ? envelope.code : ApiErrorKind.HTTP
    // 重新验证（code 23）虽然走 HTTP 401，但不是会话失效：必须留给调用方走重验证后重试。
    const reauthRequired = code === ERROR_CODES.REAUTH_REQUIRED
    const expired =
      !reauthRequired &&
      (response.status === 401 ||
        response.status === 403 ||
        code === ERROR_CODES.UNAUTHORIZED ||
        code === ERROR_CODES.FORBIDDEN)
    const apiError = new ApiError({
      status: response.status,
      code,
      message: typeof envelope.message === 'string' ? envelope.message : '',
      userMessage:
        (typeof envelope.userMessage === 'string' && envelope.userMessage) ||
        (typeof envelope.message === 'string' && envelope.message) ||
        statusUserMessage(response.status),
      requestId: typeof envelope.requestId === 'string' && envelope.requestId ? envelope.requestId : requestId,
      sessionExpired: expired,
      reauthRequired
    })
    if (apiError.sessionExpired) notifySessionExpired(apiError)
    throw apiError
  }

  return envelope.data === undefined || envelope.data === null ? {} : envelope.data
}

export const api = {
  get: (path, params, options = {}) => request(`${path}${buildQuery(params)}`, { ...options, method: 'GET' }),
  post: (path, body, options = {}) => request(path, { ...options, method: 'POST', body }),
  put: (path, body, options = {}) => request(path, { ...options, method: 'PUT', body })
}
