import { computed, ref } from 'vue'
import { toInteger } from '@/utils/format'

/**
 * 分页展示模型。
 *
 * 分页的真实状态由各 store 持有（page/pageSize/total 都来自服务端响应），
 * 这里只提供统一的“页码文案、前后页可用性、本地切片”能力，避免每个页面重复实现。
 * 对于审计日志这类服务端不返回 total 的接口，启用 `hasMore` 判定（本页满页即认为还有下一页）。
 */
export function usePagination({ pageSize = 20, initialPage = 1 } = {}) {
  const page = ref(initialPage)
  const size = ref(pageSize)
  const total = ref(0)
  /** 服务端未提供 total 时（审计日志）用本页条数推断是否还有下一页。 */
  const lastPageCount = ref(0)

  const pageCount = computed(() => Math.max(1, Math.ceil(total.value / size.value)))
  const canPrev = computed(() => page.value > 1)
  const canNext = computed(() =>
    total.value > 0 ? page.value < pageCount.value : lastPageCount.value >= size.value
  )
  const offset = computed(() => (page.value - 1) * size.value)
  const rangeLabel = computed(() => {
    if (total.value <= 0) {
      return lastPageCount.value === 0 ? '暂无记录' : `第 ${offset.value + 1}-${offset.value + lastPageCount.value} 条`
    }
    const from = offset.value + 1
    const to = Math.min(offset.value + size.value, total.value)
    return `第 ${from}-${to} 条 / 共 ${total.value} 条`
  })

  /** 从 store 同步分页状态（store 是唯一数据源）。 */
  function sync(source = {}) {
    const nextPage = toInteger(source.page)
    const nextSize = toInteger(source.pageSize)
    const nextTotal = toInteger(source.total)
    if (nextPage !== null && nextPage > 0) page.value = nextPage
    if (nextSize !== null && nextSize > 0) size.value = nextSize
    if (nextTotal !== null && nextTotal >= 0) total.value = nextTotal
    return { page: page.value, pageSize: size.value, total: total.value }
  }

  function setTotal(value) {
    total.value = toInteger(value) ?? 0
  }

  function setPage(next) {
    const value = toInteger(next)
    if (value === null || value < 1) return false
    page.value = Math.min(value, pageCount.value)
    return true
  }

  /** 记录本页实际条数，供无 total 的接口判断是否还有下一页。 */
  function observeRows(rows) {
    lastPageCount.value = Array.isArray(rows) ? rows.length : 0
    return lastPageCount.value
  }

  function reset() {
    page.value = initialPage
    total.value = 0
    lastPageCount.value = 0
  }

  return {
    page,
    pageSize: size,
    total,
    pageCount,
    canPrev,
    canNext,
    offset,
    rangeLabel,
    sync,
    setPage,
    setTotal,
    observeRows,
    reset
  }
}
