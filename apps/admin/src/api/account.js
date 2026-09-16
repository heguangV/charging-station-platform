import { api } from './http'
import { flattenPage } from './contract'

export function fetchAccounts({ page, pageSize } = {}) {
  return api.get('/admin/accounts', { page, pageSize }).then(flattenPage)
}

export function createAccount(payload, { idempotencyKey } = {}) {
  return api.post('/admin/accounts', payload, { idempotent: true, idempotencyKey })
}

export function setAccountStatus(accountId, payload, { idempotencyKey } = {}) {
  return api.put(`/admin/accounts/${encodeURIComponent(accountId)}/status`, payload, {
    idempotent: true,
    idempotencyKey
  })
}
