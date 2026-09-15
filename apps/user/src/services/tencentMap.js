/**
 * 腾讯地图 JS API 加载器：在真实网页里通过 <script> 注入官方 GL JS 库，
 * 不使用 Qt WebView / WebChannel / iframe 等任何嵌入式桥接。
 * JS Key 由构建时的 envPrefix 注入，本身就是客户端可见凭据；
 * Server Key 与 AI_* 配置绝不出现在前端。
 */

export const MAP_SCRIPT_ID = 'ncs-tencent-map-script'
const MAP_SCRIPT_SRC = 'https://map.qq.com/api/gljs?v=1&key='

export const MapLoadErrorKind = {
  MISSING_KEY: 'MISSING_KEY',
  LOAD_FAILED: 'LOAD_FAILED',
  NO_API: 'NO_API'
}

/** 地图加载失败：kind 用于分支，userMessage 可直接展示。 */
export class MapLoadError extends Error {
  constructor(kind, userMessage) {
    super(userMessage)
    this.name = 'MapLoadError'
    this.kind = kind
    this.userMessage = userMessage
  }
}

let loaderPromise = null

/** 读取客户端可见的腾讯地图 JS Key；未配置时返回空串。 */
export function getMapKey() {
  const key = import.meta.env ? import.meta.env.TENCENT_MAP_JS_KEY : ''
  return typeof key === 'string' ? key.trim() : ''
}

export function hasMapKey() {
  return getMapKey().length > 0
}

export function isMapApiReady() {
  return typeof window !== 'undefined' && !!window.TMap
}

/** 仅用于测试与手动重试：清空缓存的加载 Promise。 */
export function resetMapLoader() {
  loaderPromise = null
  if (typeof document !== 'undefined') document.getElementById(MAP_SCRIPT_ID)?.remove()
}

/**
 * 加载腾讯地图 GL JS，单次注入、并发调用共享同一 Promise。
 * 失败时抛出 MapLoadError 并清空缓存，调用方可以重试（不会静默成功）。
 */
export function loadTencentMap() {
  if (typeof window === 'undefined' || typeof document === 'undefined') {
    return Promise.reject(new MapLoadError(MapLoadErrorKind.NO_API, '当前运行环境不支持腾讯地图，请使用浏览器访问。'))
  }
  if (isMapApiReady()) return Promise.resolve(window.TMap)
  if (loaderPromise) return loaderPromise

  const key = getMapKey()
  if (!key) {
    return Promise.reject(new MapLoadError(MapLoadErrorKind.MISSING_KEY, '尚未配置腾讯地图 Key，地图暂不可用；站点列表与导航仍可正常使用。'))
  }

  loaderPromise = new Promise((resolve, reject) => {
    const previous = document.getElementById(MAP_SCRIPT_ID)
    if (previous) previous.remove()

    const script = document.createElement('script')
    script.id = MAP_SCRIPT_ID
    script.async = true
    script.src = `${MAP_SCRIPT_SRC}${encodeURIComponent(key)}`

    const fail = (kind, userMessage) => {
      script.remove()
      loaderPromise = null
      reject(new MapLoadError(kind, userMessage))
    }

    script.onload = () => {
      if (isMapApiReady()) resolve(window.TMap)
      else fail(MapLoadErrorKind.NO_API, '腾讯地图脚本已加载但未能初始化，请检查 Key 类型与来源白名单后重试。')
    }
    script.onerror = () => fail(MapLoadErrorKind.LOAD_FAILED, '无法连接腾讯地图服务，请检查网络后重试。')

    document.head.appendChild(script)
  })

  return loaderPromise
}
