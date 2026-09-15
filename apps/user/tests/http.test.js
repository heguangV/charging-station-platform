import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { existsSync, readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import {
  AGENT_TIMEOUT_MS,
  ApiError,
  ApiErrorKind,
  DEFAULT_TIMEOUT_MS,
  buildQuery,
  clearAccessToken,
  getAccessToken,
  getTokenStorageScope,
  request,
  setAccessToken,
  setSessionExpiredHandler
} from '../src/api/http'
import { avatarContentUrl, fetchAvatarObjectUrl, uploadAvatar } from '../src/api/auth'
import { chatWithAgent } from '../src/api/agent'
import { rechargeWallet } from '../src/api/charging'
import { fetchStations } from '../src/api/station'

/** 读取仓库内源文件：jsdom 环境下 import.meta.url 不是 file://，因此基于 cwd 定位。 */
function readProjectFile(relativePath) {
  const direct = resolve(process.cwd(), relativePath)
  const fallback = resolve(process.cwd(), 'apps/user', relativePath)
  return readFileSync(existsSync(direct) ? direct : fallback, 'utf8')
}

const UUID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i

function envelope(data, extra = {}) {
  return { success: true, code: 0, message: '', userMessage: '', requestId: 'req-1', data, ...extra }
}

/** 轻量响应替身：只实现 http.js 用到的字段。 */
function jsonResponse(body, status = 200) {
  return { ok: status >= 200 && status < 300, status, json: async () => body }
}

function mockFetch(handler) {
  const fetchMock = vi.fn(handler)
  vi.stubGlobal('fetch', fetchMock)
  return fetchMock
}

beforeEach(() => {
  clearAccessToken()
  setSessionExpiredHandler(null)
  sessionStorage.clear()
})

afterEach(() => {
  clearAccessToken()
  setSessionExpiredHandler(null)
  vi.unstubAllGlobals()
})

describe('统一信封与请求头', () => {
  it('成功响应解包 data，并带上 X-Request-ID 与请求前缀 /api/v1', async () => {
    const fetchMock = mockFetch(async () => jsonResponse(envelope({ balanceCent: 10000, debtCent: 0 })))
    const data = await request('/user/wallet')
    expect(data).toEqual({ balanceCent: 10000, debtCent: 0 })
    const [url, init] = fetchMock.mock.calls[0]
    expect(url).toBe('/api/v1/user/wallet')
    expect(init.method).toBe('GET')
    expect(init.headers['X-Request-ID']).toMatch(UUID_PATTERN)
    expect(init.cache).toBe('no-store')
    expect(init.signal).toBeInstanceOf(AbortSignal)
  })

  it('携带 Bearer 令牌，且令牌只镜像到 sessionStorage（不使用 localStorage）', async () => {
    const fetchMock = mockFetch(async () => jsonResponse(envelope({})))
    setAccessToken('token-abc')
    await request('/user/me')

    expect(fetchMock.mock.calls[0][1].headers.Authorization).toBe('Bearer token-abc')
    expect(getAccessToken()).toBe('token-abc')
    expect(getTokenStorageScope()).toBe('session')
    expect(sessionStorage.getItem('ncs.user.accessToken')).toBe('token-abc')

    // 源码级约束：对浏览器长期存储的任何访问都是 setItem/getItem/下标形式，这里一律禁止。
    const source = readProjectFile('src/api/http.js')
    expect(source).not.toMatch(/localStorage\s*(\.|\[)/)
  })

  it('success:false 抛出带 code/userMessage 的 ApiError，且不透出内部 message', async () => {
    mockFetch(async () =>
      jsonResponse({ success: false, code: 16, message: 'quote expired', userMessage: '报价已过期，请重新排队或预约', requestId: 'req-9', data: null }, 409)
    )
    const error = await request('/user/flows/FL1/quote-confirmations', { method: 'POST', body: {}, idempotent: true }).catch(caught => caught)
    expect(error).toBeInstanceOf(ApiError)
    expect(error.status).toBe(409)
    expect(error.code).toBe(16)
    expect(error.userMessage).toBe('报价已过期，请重新排队或预约')
    expect(error.requestId).toBe('req-9')
    expect(error.sessionExpired).toBe(false)
  })

  it('失败时缺少 userMessage 时回退到 message，再回退到状态码文案', async () => {
    mockFetch(async () => jsonResponse({ success: false, code: 13, message: 'internal error', userMessage: '', data: null }, 500))
    const error = await request('/user/orders').catch(caught => caught)
    expect(error.userMessage).toBe('internal error')

    mockFetch(async () => jsonResponse({ success: false, code: 13, message: '', userMessage: '', data: null }, 503))
    const second = await request('/user/orders').catch(caught => caught)
    expect(second.userMessage).toBe('服务暂时不可用，请稍后重试')
  })

  it('401/403 标记会话失效并触发清理回调', async () => {
    const expired = vi.fn()
    setSessionExpiredHandler(expired)
    setAccessToken('token-abc')
    mockFetch(async () => jsonResponse({ success: false, code: 401, message: 'unauthorized', userMessage: '登录已失效，请重新登录', data: null }, 401))

    const error = await request('/user/me').catch(caught => caught)
    expect(error.sessionExpired).toBe(true)
    expect(expired).toHaveBeenCalledTimes(1)
    expect(getAccessToken()).toBeNull()
    expect(sessionStorage.getItem('ncs.user.accessToken')).toBeNull()
  })

  it('超时中断请求并映射为可展示的中文提示', async () => {
    mockFetch((url, init) => {
      return new Promise((resolve, reject) => {
        // 真实 fetch 会在 signal 被中断时以 TimeoutError 拒绝，这里复现同一行为。
        init.signal.addEventListener('abort', () => reject(init.signal.reason || new DOMException('timeout', 'TimeoutError')))
      })
    })
    const error = await request('/user/wallet', { timeout: 5 }).catch(caught => caught)
    expect(error).toBeInstanceOf(ApiError)
    expect(error.code).toBe(ApiErrorKind.TIMEOUT)
    expect(error.userMessage).toContain('请求超时')
  })

  it('网络异常映射为 NETWORK 错误，而不是抛出原生异常', async () => {
    mockFetch(async () => {
      throw new TypeError('Failed to fetch')
    })
    const error = await request('/user/stations').catch(caught => caught)
    expect(error.code).toBe(ApiErrorKind.NETWORK)
    expect(error.userMessage).toContain('网络')
  })

  it('JSON 无法解析时返回 INVALID_RESPONSE，不向上暴露解析细节', async () => {
    mockFetch(async () => ({
      ok: true,
      status: 200,
      json: async () => {
        throw new SyntaxError('Unexpected token < in JSON')
      }
    }))
    const error = await request('/user/wallet').catch(caught => caught)
    expect(error.code).toBe(ApiErrorKind.INVALID_RESPONSE)
    expect(error.userMessage).not.toContain('JSON')
  })

  it('超时常量：普通请求 8 秒，AI 会话 20 秒', () => {
    expect(DEFAULT_TIMEOUT_MS).toBe(8000)
    expect(AGENT_TIMEOUT_MS).toBe(20000)
  })
})

describe('幂等键与查询参数', () => {
  it('业务写入 POST 自动携带 Idempotency-Key，并在重试时复用传入的键', async () => {
    const fetchMock = mockFetch(async () => jsonResponse(envelope({ rechargeNo: 'RC1' })))
    await rechargeWallet(10000)
    const firstHeaders = fetchMock.mock.calls[0][1].headers
    expect(firstHeaders['Idempotency-Key']).toMatch(UUID_PATTERN)
    expect(firstHeaders['Content-Type']).toBe('application/json; charset=utf-8')
    expect(fetchMock.mock.calls[0][1].body).toBe(JSON.stringify({ amountCent: 10000 }))

    await rechargeWallet(10000, 'fixed-key-1')
    expect(fetchMock.mock.calls[1][1].headers['Idempotency-Key']).toBe('fixed-key-1')
  })

  it('AI 助手会话是只读 POST，不带 Idempotency-Key，并使用更长的超时预算', async () => {
    const fetchMock = mockFetch(async () => jsonResponse(envelope({ reply: '好的', stations: [], pois: [], route: null, actions: [], tools: [], llmUsed: true, degraded: false })))
    await chatWithAgent({ message: '充电站附近有什么咖啡店', location: { latitudeE6: 39977680, longitudeE6: 116316417 }, coordinateType: 'gcj02' })

    const [url, init] = fetchMock.mock.calls[0]
    expect(url).toBe('/api/v1/user/agent/chat')
    expect(init.headers['Idempotency-Key']).toBeUndefined()
    expect(init.method).toBe('POST')
    expect(JSON.parse(init.body)).toEqual({
      message: '充电站附近有什么咖啡店',
      location: { latitudeE6: 39977680, longitudeE6: 116316417 },
      coordinateType: 'gcj02'
    })
  })


  it('头像内容需要 Bearer 令牌单独请求，未设置头像（404）时返回空串', async () => {
    const fetchMock = mockFetch(async () => ({ ok: false, status: 404, json: async () => ({ success: false, code: 4, userMessage: '未设置头像', data: null }) }))
    setAccessToken('token-abc')

    expect(avatarContentUrl()).toBe('/api/v1/user/me/avatar/content')
    await expect(fetchAvatarObjectUrl()).resolves.toBe('')
    expect(fetchMock.mock.calls[0][0]).toBe('/api/v1/user/me/avatar/content')
    expect(fetchMock.mock.calls[0][1].headers.Authorization).toBe('Bearer token-abc')

    mockFetch(async () => ({ ok: true, status: 200, blob: async () => new Blob([new Uint8Array([1, 2, 3])], { type: 'image/png' }) }))
    expect(typeof (await fetchAvatarObjectUrl())).toBe('string')
  })

  it('上传头像是业务写入：携带 Idempotency-Key 且使用 multipart 表单', async () => {
    const fetchMock = mockFetch(async () => jsonResponse(envelope({ avatarUrl: '/api/v1/user/me/avatar/content', version: 4 })))
    const file = new File([new Uint8Array([1, 2, 3])], 'avatar.png', { type: 'image/png' })

    await uploadAvatar(file)

    const [url, init] = fetchMock.mock.calls[0]
    expect(url).toBe('/api/v1/user/me/avatar')
    expect(init.method).toBe('POST')
    expect(init.headers['Idempotency-Key']).toMatch(UUID_PATTERN)
    // FormData 由浏览器自行设置 multipart boundary，不能手写 Content-Type。
    expect(init.headers['Content-Type']).toBeUndefined()
    expect(init.body).toBeInstanceOf(FormData)
  })

  it('查询参数忽略空值，且站点查询在无定位时不下发经纬度', async () => {    expect(buildQuery({ page: 1, keyword: '', chargerType: undefined, latitudeE6: null })).toBe('?page=1')
    expect(buildQuery({})).toBe('')

    const fetchMock = mockFetch(async () => jsonResponse(envelope({ items: [], total: 0, page: 1, pageSize: 20 })))
    await fetchStations({ keyword: '中关村', chargerType: 1, page: 1, pageSize: 20 })
    expect(fetchMock.mock.calls[0][0]).toBe('/api/v1/user/stations?keyword=%E4%B8%AD%E5%85%B3%E6%9D%91&chargerType=1&page=1&pageSize=20')
  })
})
