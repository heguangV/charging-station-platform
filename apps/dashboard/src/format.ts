export const kwh = (energyMwh: number): number => energyMwh / 1_000_000
export const dateLabel = (at: number): string => new Date(at * 1000).toLocaleDateString('zh-CN')
export const timeLabel = (at: number): string => new Date(at * 1000).toLocaleString('zh-CN', { month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', hour12: false })
export const escapeHtml = (value: unknown): string => String(value).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!)
