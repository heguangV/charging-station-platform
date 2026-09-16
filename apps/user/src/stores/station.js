import { defineStore } from 'pinia'
import { fetchChargers, fetchStation, fetchStationReviews, fetchStations } from '../api/station'
import { getCurrentLocation, presetLocation, GeolocationError } from '../services/geolocation'

/**
 * 站点与定位状态。经纬度始终是整数 E6 度；未知位置时不下发坐标参数。
 * 旧契约的 /quote 与 /route 在 Go 契约中不存在（单价取站点详情、路线规划待 A-07），
 * 因此这里不再维护 quote/route 状态。
 */
const createLocation = () => ({
  status: 'unknown',
  latitudeE6: null,
  longitudeE6: null,
  coordinateType: 'wgs84',
  accuracyMeter: null,
  label: '',
  source: '',
  error: ''
})

/**
 * 站点与定位状态。经纬度始终是整数 E6 度；未知位置时不下发坐标参数，
 * 由服务端按 keyword 地理编码或使用演示默认位置，并在响应里返回 locationFallback。
 */
export const useStationStore = defineStore('userStation', {
  state: () => ({
    location: createLocation(),
    stations: [],
    total: 0,
    page: 1,
    pageSize: 20,
    keyword: '',
    chargerType: null,
    locationFallback: false,
    loading: false,
    error: '',
    selectedStationId: null,
    detail: null,
    detailLoading: false,
    chargerTypeTab: 1,
    chargers: [],
    chargersLoading: false,
    reviews: [],
    reviewsLoading: false
  }),

  getters: {
    hasLocation: state => state.location.status === 'success' && Number.isInteger(state.location.latitudeE6) && Number.isInteger(state.location.longitudeE6),
    isEmpty: state => !state.loading && !state.error && state.stations.length === 0,
    selectedStation: state => state.stations.find(item => String(item.id) === String(state.selectedStationId)) || null
  },

  actions: {
    setChargerType(chargerType) {
      this.chargerType = chargerType === 0 || chargerType === 1 ? chargerType : null
    },

    setKeyword(keyword) {
      this.keyword = typeof keyword === 'string' ? keyword.trim() : ''
    },

    applyLocation(location, source) {
      this.location = { ...createLocation(), ...location, status: 'success', source: source || '' }
    },

    async locate() {
      this.location = { ...createLocation(), status: 'loading' }
      try {
        const position = await getCurrentLocation()
        this.applyLocation(position, 'geolocation')
        return position
      } catch (error) {
        const kind = error instanceof GeolocationError ? error.kind : 'UNAVAILABLE'
        this.location = {
          ...createLocation(),
          status: kind === 'UNSUPPORTED' ? 'unsupported' : 'failed',
          error: error?.userMessage || '定位失败，请手动选择位置'
        }
        return null
      }
    },

    /** 手动兜底：定位被拒后仍可用预设位置搜索站点与规划路线。 */
    usePresetLocation(presetId) {
      const preset = presetLocation(presetId)
      this.applyLocation(preset, 'preset')
      return preset
    },

    clearLocation() {
      this.location = createLocation()
    },

    /** 附近站点查询；位置未知时省略经纬度，只提交 keyword。 */
    async search({ chargerType, keyword, page = 1 } = {}) {
      if (chargerType !== undefined) this.setChargerType(chargerType)
      if (keyword !== undefined) this.setKeyword(keyword)
      this.loading = true
      this.error = ''
      const latitudeE6 = this.hasLocation ? this.location.latitudeE6 : undefined
      const longitudeE6 = this.hasLocation ? this.location.longitudeE6 : undefined
      try {
        const data = await fetchStations({
          latitudeE6,
          longitudeE6,
          keyword: this.keyword || undefined,
          chargerType: this.chargerType === null ? undefined : this.chargerType,
          page,
          pageSize: this.pageSize
        })
        const items = Array.isArray(data.items) ? data.items : []
        this.stations = items
        this.total = Number.isInteger(data.total) ? data.total : items.length
        this.page = Number.isInteger(data.page) ? data.page : page
        this.locationFallback = data.locationFallback === true
        return items
      } catch (error) {
        this.stations = []
        this.total = 0
        this.error = error?.userMessage || '站点加载失败，请稍后重试'
        return []
      } finally {
        this.loading = false
      }
    },

    async loadStation(stationId) {
      this.selectedStationId = stationId
      this.detailLoading = true
      try {
        this.detail = await fetchStation(stationId)
        return this.detail
      } catch (error) {
        this.detail = null
        this.error = error?.userMessage || '站点详情加载失败'
        return null
      } finally {
        this.detailLoading = false
      }
    },

    async loadChargers(stationId, { chargerType } = {}) {
      if (chargerType !== undefined) this.chargerTypeTab = chargerType
      this.chargersLoading = true
      try {
        const data = await fetchChargers(stationId, { chargerType: this.chargerTypeTab, pageSize: 50 })
        this.chargers = Array.isArray(data.items) ? data.items : []
        return this.chargers
      } catch (error) {
        this.chargers = []
        this.error = error?.userMessage || '设备列表加载失败'
        return []
      } finally {
        this.chargersLoading = false
      }
    },

    async loadReviews(stationId) {
      this.reviewsLoading = true
      try {
        const data = await fetchStationReviews(stationId)
        this.reviews = Array.isArray(data.items) ? data.items : []
        return this.reviews
      } catch (error) {
        this.reviews = []
        return []
      } finally {
        this.reviewsLoading = false
      }
    }
  }
})
