/**
 * 跳转式导航：腾讯地图 URI API（routeplan）。
 *
 * 服务端路线规划（A-07）尚未接入 Go 后端，站内画线导航暂不可行；URI API 是
 * 腾讯官方公开的网页跳转协议，不需要任何 Key，也不把服务端凭据暴露给浏览器——
 * 这与仓库基线一致：客户端可见的只有地图 JS Key（本就在前端注入）。
 *
 * 坐标一律传 GCJ-02 十进制度（coord_type=1），与腾讯地图底图同系；浏览器定位
 * 的 WGS-84 由调用方先经 coordinate.wgs84ToGcj02 换算。from 缺省时省略起点，
 * 由腾讯地图在设备端使用当前位置。
 */

export const NAVIGATION_MODES = [
  { value: 'driving', label: '驾车' },
  { value: 'walking', label: '步行' }
]

const ROUTEPLAN_URL = 'https://apis.map.qq.com/uri/v1/routeplan'

/**
 * 构建腾讯地图路线规划跳转链接。
 * @param {object} options
 * @param {{latitude:number, longitude:number, name?:string}|null} options.from 起点（GCJ-02），null 表示由地图使用当前位置
 * @param {{latitude:number, longitude:number, name?:string}} options.to 终点（GCJ-02），必填
 * @param {'driving'|'walking'} [options.mode] 出行方式，默认驾车
 * @param {string} [options.referer] URI API 的应用标识
 * @returns {string} 可直接 window.open 的 URL
 */
export function buildRoutePlanUrl({ from, to, mode = 'driving', referer = 'ncs-user-web' } = {}) {
  if (!NAVIGATION_MODES.some(item => item.value === mode)) {
    throw new RangeError(`不支持的出行方式：${mode}`)
  }
  if (!to || !Number.isFinite(to.latitude) || !Number.isFinite(to.longitude)) {
    throw new RangeError('终点坐标缺失，无法规划路线')
  }
  if (from && (!Number.isFinite(from.latitude) || !Number.isFinite(from.longitude))) {
    throw new RangeError('起点坐标不完整，请省略起点或补全坐标')
  }

  const params = new URLSearchParams()
  params.set('mode', mode)
  if (from) {
    params.set('from', from.name || '我的位置')
    params.set('fromcoord', `${from.latitude},${from.longitude}`)
  }
  params.set('to', to.name || '目的地')
  params.set('tocoord', `${to.latitude},${to.longitude}`)
  // 腾讯地图底图与站点坐标同为 GCJ-02；显式声明，避免被当作 WGS-84 再次偏移。
  params.set('coord_type', '1')
  params.set('referer', referer)
  return `${ROUTEPLAN_URL}?${params.toString()}`
}

export function navigationModeLabel(mode) {
  const found = NAVIGATION_MODES.find(item => item.value === mode)
  return found ? found.label : ''
}
