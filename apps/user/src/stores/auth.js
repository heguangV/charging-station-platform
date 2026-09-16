import { defineStore } from 'pinia'
import * as authApi from '../api/auth'
import { fetchWallet } from '../api/charging'
import { clearAccessToken, getAccessToken, setAccessToken, setSessionExpiredHandler } from '../api/http'

/**
 * 认证状态：令牌只保存在这里并镜像到 sessionStorage（见 api/http.js 的说明）。
 * 资料由 api/auth.js 聚合 /me、/me/profile 与 /wallet 组成；Go 契约没有
 * 欠费模型与资料乐观锁，余额以后端返回为准，昵称修改不再提交 version。
 */
export const useAuthStore = defineStore('userAuth', {
  state: () => ({
    token: getAccessToken(),
    expiresAt: 0,
    user: null,
    profile: null,
    wallet: null,
    loading: false,
    codeLoading: false,
    error: null,
    notice: '',
    sessionExpired: false,
    /** 仅开发环境返回的模拟验证码，用于联调提示；生产响应中没有该字段。 */
    developmentCode: ''
  }),

  getters: {
    isLoggedIn: state => !!state.token,
    balanceCent: state => state.profile?.balanceCent ?? state.wallet?.balanceCent ?? 0,
    debtCent: state => state.profile?.debtCent ?? state.wallet?.debtCent ?? 0,
    availableCent: state => state.wallet?.availableCent ?? Math.max(0, (state.profile?.balanceCent ?? 0) - (state.profile?.debtCent ?? 0)),
    hasDebt: state => (state.profile?.debtCent ?? 0) > 0,
    hasActiveFlow: state => state.profile?.hasActiveFlow === true,
    displayName: state => state.user?.displayName || state.user?.nickname || state.user?.username || '未登录',
    phoneMasked: state => state.user?.phoneMasked || '',
    avatarUrl: state => state.user?.avatarUrl || ''
  },

  actions: {
    /** 401/403 由 http 层判定，这里统一清理会话状态。 */
    bindSessionExpiry() {
      setSessionExpiredHandler(error => {
        this.token = null
        this.expiresAt = 0
        this.sessionExpired = true
        this.error = error?.userMessage || '登录已失效，请重新登录'
      })
    },

    applySession(session) {
      if (!session || typeof session.accessToken !== 'string' || !session.accessToken) {
        throw new Error('登录响应缺少 accessToken')
      }
      this.token = session.accessToken
      this.expiresAt = Number.isInteger(session.expiresAt) ? session.expiresAt : 0
      this.user = session.user || null
      this.sessionExpired = false
      this.error = null
      setAccessToken(this.token)
      this.bindSessionExpiry()
      return session
    },

    async requestCode(phone) {
      this.codeLoading = true
      this.error = null
      this.notice = ''
      try {
        const data = await authApi.sendSmsCode(phone)
        this.developmentCode = typeof data.developmentCode === 'string' ? data.developmentCode : ''
        this.notice = '验证码已发送'
        return data
      } catch (error) {
        this.handleError(error)
        return null
      } finally {
        this.codeLoading = false
      }
    },

    async loginWithSms({ phone, smsCode }) {
      return this.runLogin(() => authApi.loginWithSms({ phone, smsCode }))
    },

    async loginWithPassword({ loginName, password }) {
      return this.runLogin(() => authApi.loginWithPassword({ loginName, password }))
    },

    /** 用户名密码注册（Go 契约 201 返回登录会话，注册即登录）。 */
    async register({ username, phone, password, smsCode }) {
      return this.runLogin(() => authApi.registerAccount({ username, phone, password, smsCode }))
    },

    async runLogin(call) {
      this.loading = true
      this.error = null
      this.notice = ''
      try {
        this.applySession(await call())
        this.notice = '登录成功'
        await Promise.all([this.refreshProfile(), this.refreshWallet()])
        return true
      } catch (error) {
        this.handleError(error)
        return false
      } finally {
        this.loading = false
      }
    },

    async refreshProfile() {
      if (!this.token) return null
      try {
        this.profile = await authApi.fetchProfile()
        if (this.profile?.user) this.user = this.profile.user
        else if (this.profile?.id) this.user = this.profile
        return this.profile
      } catch (error) {
        this.handleError(error)
        return null
      }
    },

    async refreshWallet() {
      if (!this.token) return null
      try {
        this.wallet = await fetchWallet()
        return this.wallet
      } catch (error) {
        this.handleError(error)
        return null
      }
    },

    async updateNickname(nickname) {
      this.loading = true
      try {
        this.profile = await authApi.updateNickname(nickname)
        if (this.profile?.user) this.user = this.profile.user
        this.notice = '昵称已更新'
        this.error = null
        return true
      } catch (error) {
        this.handleError(error)
        return false
      } finally {
        this.loading = false
      }
    },

    /** 头像地址（Go 契约 avatarUrl 字符串；无文件上传端点）。 */
    async updateAvatarUrl(avatarUrl) {
      this.loading = true
      try {
        this.profile = await authApi.updateAvatarUrl(avatarUrl)
        if (this.profile?.user) this.user = this.profile.user
        this.notice = avatarUrl ? '头像地址已更新' : '已清除头像地址'
        this.error = null
        return true
      } catch (error) {
        this.handleError(error)
        return false
      } finally {
        this.loading = false
      }
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
      this.user = null
      this.profile = null
      this.wallet = null
      this.developmentCode = ''
      this.sessionExpired = false
      clearAccessToken()
      this.notice = message
    },

    handleSessionExpired(message = '登录已失效，请重新登录') {
      this.token = null
      this.expiresAt = 0
      this.sessionExpired = true
      this.error = message
      clearAccessToken()
    },

    handleError(error) {
      if (error?.sessionExpired) {
        this.handleSessionExpired(error.userMessage)
        return
      }
      this.error = error?.userMessage || '操作失败，请稍后重试'
    },

    /** 页面刷新后从 sessionStorage 恢复令牌并拉取资料。 */
    async restore() {
      this.bindSessionExpiry()
      const token = getAccessToken()
      if (!token) return false
      this.token = token
      await Promise.all([this.refreshProfile(), this.refreshWallet()])
      return this.isLoggedIn
    }
  }
})
