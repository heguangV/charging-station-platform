import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import {
  API_BASE,
  ApiError,
  ApiErrorKind,
  ERROR_CODES,
  TOKEN_STORAGE_KEY,
  api,
  buildQuery,
  clearAccessToken,
  getAccessToken,
  getTokenStorageScope,
  randomId,
  request,
  setAccessToken,
  setSessionExpiredHandler
} from '../src/api/http'
import { failResponse, installFetch, okResponse } from './helpers'

let harness = null

beforeEach(() => {
  clearAccessToken()
  setSessionExpiredHandler(null)
  sessionStorage.clear()
  localStorage.clear()
})

afterEach(() => {
  vi.unstubAllGlobals()
  clearAccessToken()
  setSessionExpiredHandler(null)
})

describe('信封解包', () => {
  it('成功响应返回 data，data 缺失时返回空对象', async () => {
    harness = installFetch([okResponse({ items: [{ id: 1 }], total: 1 }), okResponse(null)])
    await expect(request('/admin/stations')).resolves.toEqual({ items: [{ id: 1 }], total: 1 })
    await expect(request('/admin/stations')).resolves.toEqual({})
  })

  it('success:false 抛出带 code 与 userMessage 的 ApiError', async () => {
    harness = installFetch([failResponse({ status: 422, code: 2, userMessage: '提交内容不符合要求' })])
    await expect(request('/admin/stations')).rejects.toBeInstanceOf(ApiError)
    try {
      await request('/admin/stations')
    } catch (error) {
      expect(error.code).toBe(ERROR_CODES.VALIDATION_FAILED)
      expect(error.status).toBe(422)
      expect(error.userMessage).toBe('提交内容不符合要求')
      expect(error.requestId).toBe('req-err')
      expect(error.sessionExpired).toBe(false)
    }
  })

  it('响应不是 JSON 信封时按无效响应处理', async () => {
    harness = installFetch([{ ok: false, status: 502, json: async () => { throw new Error('boom') } }])
    await expect(request('/admin/stations')).rejects.toMatchObject({
      code: ApiErrorKind.INVALID_RESPONSE,
      status: 502
    })
  })
})

describe('会话失效与重新验证的区分', () => {
  it('HTTP 401 视为会话失效，清理令牌并回调', async () => {
    setAccessToken('token-1')
    const onExpired = vi.fn()
    setSessionExpiredHandler(onExpired)
    harness = installFetch([failResponse({ status: 401, code: 401, userMessage: '登录已失效' })])

    await expect(request('/admin/stations')).rejects.toMatchObject({ sessionExpired: true })
    expect(onExpired).toHaveBeenCalledTimes(1)
    expect(getAccessToken()).toBeNull()
    expect(sessionStorage.getItem(TOKEN_STORAGE_KEY)).toBeNull()
  })

  it('HTTP 403 同样视为会话失效', async () => {
    setAccessToken('token-2')
    harness = installFetch([failResponse({ status: 403, code: 403, userMessage: '需要 OWNER 权限' })])
    await expect(request('/admin/accounts')).rejects.toMatchObject({ sessionExpired: true, code: 403 })
  })

  it('REAUTH_REQUIRED(23) 走 HTTP 401 但不是会话失效', async () => {
    setAccessToken('token-3')
    const onExpired = vi.fn()
    setSessionExpiredHandler(onExpired)
    harness = installFetch([failResponse({ status: 401, code: 23, userMessage: '需要重新验证管理员密码' })])

    await expect(request('/admin/backups')).rejects.toMatchObject({
      code: ERROR_CODES.REAUTH_REQUIRED,
      reauthRequired: true,
      sessionExpired: false
    })
    expect(onExpired).not.toHaveBeenCalled()
    // 令牌必须保留：重新验证后还要用同一个会话重试原请求。
    expect(getAccessToken()).toBe('token-3')
  })
})

describe('请求头与幂等键', () => {
  it('携带 Authorization 与 X-Request-ID，业务写入带 Idempotency-Key，查询不带', async () => {
    setAccessToken('token-abc')
    harness = installFetch([okResponse({}), okResponse({}), okResponse({})])

    await request('/admin/stations')
    await request('/admin/stations/1/disable', { method: 'POST', body: { reason: '维护', version: 1 }, idempotent: true })
    await request('/admin/tariffs', { method: 'POST', body: { reason: '年度价格' }, idempotent: true, idempotencyKey: 'fixed-key' })

    const getHeaders = harness.headersOf(0)
    expect(getHeaders.Authorization).toBe('Bearer token-abc')
    expect(getHeaders['X-Request-ID']).toMatch(/^[0-9a-f-]{36}$/)
    expect(getHeaders['Idempotency-Key']).toBeUndefined()

    expect(harness.headersOf(1)['Idempotency-Key']).toMatch(/^[0-9a-f-]{36}$/)
    expect(harness.headersOf(2)['Idempotency-Key']).toBe('fixed-key')
    expect(harness.headersOf(1)['Content-Type']).toBe('application/json; charset=utf-8')
  })

  it('令牌只镜像到 sessionStorage，不写 localStorage', () => {
    expect(getTokenStorageScope()).toBe('session')
    setAccessToken('session-only-token')
    expect(sessionStorage.getItem(TOKEN_STORAGE_KEY)).toBe('session-only-token')
    expect(localStorage.getItem(TOKEN_STORAGE_KEY)).toBeNull()
    expect(localStorage.length).toBe(0)
  })
})

describe('路径、查询参数与超时', () => {
  it('所有请求都指向 /api/v1/admin 前缀', async () => {
    harness = installFetch([okResponse({})])
    await request('/admin/stats/revenue')
    expect(harness.urlOf(0)).toBe(`${API_BASE}/admin/stats/revenue`)
    expect(API_BASE).toBe('/api/v1')
  })

  it('api.get 过滤空值并拼装查询串', async () => {
    harness = installFetch([okResponse({})])
    await api.get('/admin/chargers', { stationId: 3, status: 0, keyword: '', page: 1 })
    const query = harness.queryOf(0)
    expect(query.get('stationId')).toBe('3')
    expect(query.get('status')).toBe('0')
    expect(query.has('keyword')).toBe(false)
    expect(buildQuery({ a: null, b: undefined, c: '' })).toBe('')
  })

  it('使用 AbortSignal 并在超时时映射为 TIMEOUT 错误', async () => {
    harness = installFetch([
      (url, options) => {
        expect(options.signal).toBeDefined()
        const timeoutError = new Error('请求超时')
        timeoutError.name = 'TimeoutError'
        throw timeoutError
      }
    ])
    await expect(request('/admin/stations')).rejects.toMatchObject({
      code: ApiErrorKind.TIMEOUT,
      userMessage: '请求超时，请检查网络后重试'
    })
  })

  it('网络异常映射为 NETWORK 错误', async () => {
    harness = installFetch([new Error('network down')])
    await expect(request('/admin/stations')).rejects.toMatchObject({ code: ApiErrorKind.NETWORK })
  })

  it('randomId 生成 UUID，重复调用不重复', () => {
    const first = randomId()
    const second = randomId()
    expect(first).toMatch(/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/)
    expect(first).not.toBe(second)
  })
})
