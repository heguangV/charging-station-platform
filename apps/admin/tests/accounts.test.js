import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useAccountsStore } from '../src/stores/accounts'
import { installFetch, okResponse } from './helpers'

const ACCOUNT = {
  id: 8,
  username: 'night_operator',
  roles: ['OPERATOR'],
  status: 1,
  mustChangePassword: true,
  version: 1
}

beforeEach(() => {
  sessionStorage.clear()
  setActivePinia(createPinia())
})

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('管理员账号接口', () => {
  it('加载、创建和停用都访问真实 Go 接口', async () => {
    const harness = installFetch([
      okResponse({ items: [ACCOUNT], meta: { page: 1, pageSize: 20, total: 1 } }),
      okResponse(ACCOUNT),
      okResponse({ items: [ACCOUNT], meta: { page: 1, pageSize: 20, total: 1 } }),
      okResponse({ ...ACCOUNT, status: 0, version: 2 })
    ])
    const accounts = useAccountsStore()

    await expect(accounts.load()).resolves.toBe(true)
    expect(harness.indexOf('GET', '/admin/accounts')).toBe(0)
    expect(accounts.items).toEqual([ACCOUNT])

    await expect(accounts.create({
      username: 'night_operator', password: 'Initial-Password-01', reason: '补充夜班运营账号'
    })).resolves.toBe(true)
    const createIndex = harness.indexOf('POST', '/admin/accounts')
    expect(createIndex).toBeGreaterThanOrEqual(0)
    expect(harness.headersOf(createIndex)['Idempotency-Key']).toBeTruthy()

    await expect(accounts.setStatus(ACCOUNT, 0, '值班周期结束')).resolves.toBe(true)
    const statusIndex = harness.indexOf('PUT', '/admin/accounts/8/status')
    expect(statusIndex).toBeGreaterThanOrEqual(0)
    expect(harness.bodyOf(statusIndex)).toEqual({ status: 0, reason: '值班周期结束', version: 1 })
    expect(accounts.items[0].status).toBe(0)
    expect(accounts.items[0].version).toBe(2)
  })
})
