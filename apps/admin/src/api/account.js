/**
 * 管理员账号接口：Go 契约暂无管理员账号管理域（列表/创建/停启用均缺，
 * 待 B-01 契约决策）。保留原函数签名并显式抛出不可用，页面据此门控。
 */

export function fetchAccounts() {
  return unsupported('管理员账号管理在 Go 后端暂未提供（待 B-01 契约决策）')
}

export function createAccount() {
  return unsupported('创建管理员账号在 Go 后端暂未提供（待 B-01 契约决策）')
}

export function setAccountStatus() {
  return unsupported('管理员账号启停在 Go 后端暂未提供（待 B-01 契约决策）')
}
