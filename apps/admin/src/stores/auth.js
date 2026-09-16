import { defineStore } from 'pinia'
import * as authApi from '../api/auth'
import {
  ERROR_CODES,
  clearAccessToken,
  getAccessToken,
  randomId,
  setAccessToken,
  setSessionExpiredHandler
} from '../api/http'

/**
 * 管理员认证与重新验证状态。
 *
 * 令牌只保存在这里并镜像到 sessionStorage（见 api/http.js），绝不写入 localStorage。
 * 登录锁定沿用 SRS UC-A-01：任一账号连续失败 5 次锁定 30 秒，服务端返回 RATE_LIMITED(429)，
 * 客户端只负责展示倒计时，不在前端做任何“猜密码”逻辑。
 */

/** 服务端锁定窗口（秒），与 core/application/admin_auth_service.cpp 的 30 秒一致。 */
export const LOGIN_LOCK_SECONDS = 30

/** 会话级管理员资料快照：只含 id/username/roles 等非凭据信息，用于刷新后恢复导航身份。 */
const PROFILE_STORAGE_KEY = 'ncs.admin.profile'

/** 重新验证的 Promise 与密码提供者放在模块作用域，避免被响应式包裹。 */
let reauthPromise = null
let reauthResolver = null
let reauthProvider = null
let lockTimer = null

function readProfile() {
  try {
    const raw = sessionStorage.getItem(PROFILE_STORAGE_KEY)
    const parsed = raw ? JSON.parse(raw) : null
    return parsed && typeof parsed === 'object' ? parsed : null
  } catch {
    return null
  }
}

function writeProfile(admin) {
  try {
    if (admin) sessionStorage.setItem(PROFILE_STORAGE_KEY, JSON.stringify(admin))
    else sessionStorage.removeItem(PROFILE_STORAGE_KEY)
  } catch {
    /* 私密模式下不可写；内存快照仍然有效。 */
  }
}

export const useAuthStore = defineStore('adminAuth', {
  state: () => ({
    token: getAccessToken(),
    expiresAt: 0,
    admin: readProfile(),
    deviceId: 'ncs-admin-web',
    loading: false,
    error: '',
    notice: '',
    sessionExpired: false,
    /** 登录锁定的剩余秒数，> 0 时登录按钮禁用。 */
    lockedSeconds: 0,
    /** 重新验证弹窗状态。 */
    reauthActive: false,
    reauthError: '',
    reauthLoading: false,
    reauthExpiresAt: 0
  }),

  getters: {
    isLoggedIn: state => !!state.token,
    displayName: state => state.admin?.username || '未登录',
    roles: state => (Array.isArray(state.admin?.roles) ? state.admin.roles : []),
    /** Go 写角色：SUPER_ADMIN 与 OPERATOR 可执行运营写操作，AUDITOR 只读。 */
    canWrite: state => {
      const roles = Array.isArray(state.admin?.roles) ? state.admin.roles : []
      return roles.includes('SUPER_ADMIN') || roles.includes('OPERATOR')
    },
    isSuperAdmin: state => (Array.isArray(state.admin?.roles) ? state.admin.roles.includes('SUPER_ADMIN') : false),
    isOwner: state => (Array.isArray(state.admin?.roles) ? state.admin.roles.includes('OWNER') : false),
    mustChangePassword: state => state.admin?.mustChangePassword === true,
    isLocked: state => state.lockedSeconds > 0
  },

  actions: {
    /** 401/403 由 http 层判定，这里统一清理会话状态。 */
    bindSessionExpiry() {
      setSessionExpiredHandler(error => {
        this.token = null
        this.expiresAt = 0
        this.sessionExpired = true
        this.error = error?.userMessage || '登录已失效，请重新登录'
        writeProfile(null)
      })
    },

    applySession(session) {
      if (!session || typeof session.accessToken !== 'string' || !session.accessToken) {
        throw new Error('登录响应缺少 accessToken')
      }
      this.token = session.accessToken
      this.expiresAt = Number.isInteger(session.expiresAt) ? session.expiresAt : 0
      this.admin = session.admin || null
      this.sessionExpired = false
      this.error = ''
      setAccessToken(this.token)
      writeProfile(this.admin)
      this.bindSessionExpiry()
      return session
    },

    async login({ account, password }) {
      if (this.lockedSeconds > 0) {
        this.error = `账号已锁定，请 ${this.lockedSeconds} 秒后重试`
        return false
      }
      this.loading = true
      this.error = ''
      this.notice = ''
      try {
        // account 是 Go 契约的字段名（用户名为账号的一种）；deviceId 仅用于会话审计，
        // 不在 Go 契约内，服务端会忽略未知字段。
        const data = await authApi.login({ account, password, deviceId: this.deviceId })
        this.applySession(data)
        this.notice = this.mustChangePassword ? '首次登录，请先修改初始密码' : '登录成功'
        this.clearLock()
        return true
      } catch (error) {
        this.handleError(error)
        return false
      } finally {
        this.loading = false
      }
    },

    /** 登录锁定倒计时：只在 UI 上禁用提交，服务端仍然是唯一判定者。 */
    startLock(seconds = LOGIN_LOCK_SECONDS) {
      const total = Number.isInteger(seconds) && seconds > 0 ? seconds : LOGIN_LOCK_SECONDS
      this.lockedSeconds = total
      if (lockTimer) clearInterval(lockTimer)
      lockTimer = setInterval(() => {
        this.lockedSeconds = Math.max(0, this.lockedSeconds - 1)
        if (this.lockedSeconds === 0) this.clearLock()
      }, 1000)
    },

    clearLock() {
      if (lockTimer) clearInterval(lockTimer)
      lockTimer = null
      this.lockedSeconds = 0
    },

    async logout() {
      try {
        if (this.token) await authApi.logout()
      } catch {
        /* 退出失败也要清理本地会话，避免残留令牌。 */
      }
      this.clearSession('已退出登录')
    },

    clearSession(message = '') {
      this.token = null
      this.expiresAt = 0
      this.admin = null
      this.sessionExpired = false
      this.reauthExpiresAt = 0
      this.error = ''
      this.notice = message
      setAccessToken(null)
      writeProfile(null)
      this.clearLock()
    },

    handleSessionExpired(message = '登录已失效，请重新登录') {
      this.token = null
      this.expiresAt = 0
      this.admin = null
      this.sessionExpired = true
      this.error = message
      writeProfile(null)
      clearAccessToken()
    },

    handleError(error) {
      if (error?.code === ERROR_CODES.RATE_LIMITED) {
        // 连续失败 5 次 → 服务端锁定 30 秒；提示不区分账号与密码，避免枚举。
        this.startLock(LOGIN_LOCK_SECONDS)
        this.error = `账号或密码错误次数过多，已锁定 ${LOGIN_LOCK_SECONDS} 秒，请稍后再试`
        return
      }
      if (error?.sessionExpired) {
        this.handleSessionExpired(error.userMessage)
        return
      }
      this.error = error?.userMessage || '操作失败，请稍后重试'
    },

    /** 重新验证的密码来源：测试或自动化可注入 provider，避免依赖弹窗时序。 */
    setReauthProvider(provider) {
      reauthProvider = typeof provider === 'function' ? provider : null
    },

    /** 打开重新验证弹窗并等待密码；返回 null 表示管理员取消。 */
    async ensureReauth() {
      if (reauthProvider) return await reauthProvider()
      if (!reauthPromise) {
        reauthPromise = new Promise(resolve => {
          reauthResolver = resolve
        })
      }
      this.reauthActive = true
      this.reauthError = ''
      const password = await reauthPromise
      this.reauthActive = false
      return password
    },

    /** 由 ReauthDialog 调用；密码为空时保留弹窗并提示。 */
    submitReauth(password) {
      const value = typeof password === 'string' ? password : ''
      if (value === '') {
        this.reauthError = '请输入当前登录密码'
        return false
      }
      const resolve = reauthResolver
      reauthResolver = null
      reauthPromise = null
      this.reauthActive = false
      this.reauthError = ''
      if (resolve) resolve(value)
      return true
    },

    cancelReauth() {
      const resolve = reauthResolver
      reauthResolver = null
      reauthPromise = null
      this.reauthActive = false
      this.reauthError = ''
      if (resolve) resolve(null)
    },

    /** §6.2 提交密码换取 15 分钟重新验证窗口。 */
    async reauthenticate(password) {
      this.reauthLoading = true
      this.reauthError = ''
      try {
        const data = await authApi.reauth(password)
        this.reauthExpiresAt = Number.isInteger(data.reauthExpiresAt) ? data.reauthExpiresAt : 0
        this.notice = '身份已重新验证，15 分钟内可继续敏感操作'
        return true
      } catch (error) {
        this.reauthError = error?.userMessage || '重新验证失败，请重试'
        throw error
      } finally {
        this.reauthLoading = false
      }
    },

    /**
     * 敏感操作统一入口：先按正常流程发起，服务端返回 REAUTH_REQUIRED 时
     * 弹窗重新验证，然后用**同一个幂等键**重试一次（§1.8 要求超时/失败重试不得换键）。
     */
    async runWithReauth(invoke) {
      const idempotencyKey = randomId()
      try {
        return await invoke({ idempotencyKey })
      } catch (error) {
        if (error?.code !== ERROR_CODES.REAUTH_REQUIRED) throw error
        const password = await this.ensureReauth()
        if (!password) throw error
        await this.reauthenticate(password)
        return invoke({ idempotencyKey })
      }
    },

    /** §6.11 修改本人密码：成功后清除 mustChangePassword 标记。 */
    async changeOwnPassword({ currentPassword, newPassword }) {
      const data = await this.runWithReauth(() => authApi.changeOwnPassword({ currentPassword, newPassword }))
      this.admin = { ...(this.admin || {}), ...data, mustChangePassword: false }
      writeProfile(this.admin)
      this.notice = '密码已更新'
      return true
    },

    /** 页面刷新后从 sessionStorage 恢复令牌与资料快照。 */
    restore() {
      this.bindSessionExpiry()
      const token = getAccessToken()
      if (!token) return false
      this.token = token
      this.admin = readProfile()
      if (this.admin) return true
      authApi.currentIdentity().then(identity => {
        this.admin = identity
        writeProfile(this.admin)
      }).catch(() => {
        // 令牌可能已过期；请求层会清理令牌并触发登录跳转。
      })
      // 令牌本身仍可用于路由守卫；身份资料会在请求完成后补齐。
      return true
    }
  }
})
