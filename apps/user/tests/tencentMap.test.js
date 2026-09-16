import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { MAP_SCRIPT_ID, MapLoadError, MapLoadErrorKind, getMapKey, hasMapKey, loadTencentMap, resetMapLoader } from '../src/services/tencentMap'

const fakeTMap = { Map: class {}, MultiMarker: class {}, LatLng: class {}, MarkerStyle: class {} }

function injectedScript() {
  return document.getElementById(MAP_SCRIPT_ID)
}

beforeEach(() => {
  resetMapLoader()
  delete window.TMap
  vi.stubEnv('TENCENT_MAP_JS_KEY', 'js-key-1')
})

afterEach(() => {
  resetMapLoader()
  delete window.TMap
  vi.unstubAllEnvs()
})

describe('腾讯地图 JS API 加载器', () => {
  it('注入官方 GL JS 脚本，window.TMap 出现后解析，并发调用只注入一次', async () => {
    const first = loadTencentMap()
    const second = loadTencentMap()

    const scripts = document.querySelectorAll(`script#${MAP_SCRIPT_ID}`)
    expect(scripts).toHaveLength(1)
    expect(scripts[0].src).toBe('https://map.qq.com/api/gljs?v=1&key=js-key-1')
    expect(scripts[0].async).toBe(true)

    window.TMap = fakeTMap
    scripts[0].dispatchEvent(new Event('load'))

    await expect(first).resolves.toBe(fakeTMap)
    await expect(second).resolves.toBe(fakeTMap)
    expect(document.querySelectorAll(`script#${MAP_SCRIPT_ID}`)).toHaveLength(1)

    // 已就绪时不再重复注入脚本。
    await expect(loadTencentMap()).resolves.toBe(fakeTMap)
    expect(document.querySelectorAll(`script#${MAP_SCRIPT_ID}`)).toHaveLength(1)
  })

  it('脚本加载失败时抛出带中文提示的 MapLoadError，并可重试', async () => {
    const pending = loadTencentMap()
    injectedScript().dispatchEvent(new Event('error'))

    const error = await pending.catch(caught => caught)
    expect(error).toBeInstanceOf(MapLoadError)
    expect(error.kind).toBe(MapLoadErrorKind.LOAD_FAILED)
    expect(error.userMessage).toContain('无法连接腾讯地图服务')
    // 失败的脚本会被移除，重试时重新注入。
    expect(injectedScript()).toBeNull()

    const retry = loadTencentMap()
    const retryScript = injectedScript()
    expect(retryScript).not.toBeNull()
    window.TMap = fakeTMap
    retryScript.dispatchEvent(new Event('load'))
    await expect(retry).resolves.toBe(fakeTMap)
  })

  it('脚本加载完成但没有 window.TMap 时判定为 NO_API，不会静默成功', async () => {
    const pending = loadTencentMap()
    injectedScript().dispatchEvent(new Event('load'))
    const error = await pending.catch(caught => caught)
    expect(error.kind).toBe(MapLoadErrorKind.NO_API)
    expect(error.userMessage).toContain('未能初始化')
  })

  it('缺少 JS Key 时立即拒绝（不会挂起），且不注入脚本', async () => {
    vi.stubEnv('TENCENT_MAP_JS_KEY', '')
    expect(hasMapKey()).toBe(false)

    const error = await loadTencentMap().catch(caught => caught)
    expect(error).toBeInstanceOf(MapLoadError)
    expect(error.kind).toBe(MapLoadErrorKind.MISSING_KEY)
    expect(error.userMessage).toContain('尚未配置腾讯地图 Key')
    expect(injectedScript()).toBeNull()
  })

  it('Key 读取时去除首尾空白，且不引用任何服务端密钥变量', () => {
    vi.stubEnv('TENCENT_MAP_JS_KEY', '  spaced-key  ')
    expect(getMapKey()).toBe('spaced-key')

    // 前端源码中不得出现 TENCENT_MAP_SERVER_KEY 或 AI_* 配置引用。
    const sources = import.meta.glob('../src/**/*.{js,vue}', { query: '?raw', import: 'default', eager: true })
    for (const [path, source] of Object.entries(sources)) {
      expect(source, path).not.toContain('TENCENT_MAP_SERVER_KEY')
      expect(source, path).not.toMatch(/\bAI_API_KEY\b|\bAI_BASE_URL\b|\bAI_MODEL\b|\bAI_PROVIDER\b/)
    }
  })
})
