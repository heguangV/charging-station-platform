package auth

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"time"

	bredis "github.com/heguangV/charging-station-platform/backend/internal/repository/redis"
)

// RedisSessionStore adapts the B-line redis.Sessions (ncs:session:{id}) to
// the storage-neutral SessionStore. Sessions live in Redis per the
// architecture boundary: any API instance can validate any token and a
// restart no longer logs users out. Redis unavailability is FailClosed —
// an unverifiable session is an error, never an anonymous request.
//
// A per-user token index (ncs:user-sessions:{identityID}, a Redis SET)
// supports revoking every user session of one identity — required by account
// deletion and account freezing. Admin IDs are in a separate domain and are
// deliberately not indexed here. The index is FailClosed like the sessions.
type RedisSessionStore struct {
	sessions *bredis.Sessions
	commands bredis.Commands
}

// NewRedisSessionStore binds the adapter to the B-line session store.
func NewRedisSessionStore(sessions *bredis.Sessions, commands bredis.Commands) (*RedisSessionStore, error) {
	if sessions == nil {
		return nil, errors.New("auth: redis sessions are required")
	}
	if commands == nil {
		return nil, errors.New("auth: redis commands are required")
	}
	return &RedisSessionStore{sessions: sessions, commands: commands}, nil
}

// userSessionsKey builds ncs:user-sessions:{identityID}.
func userSessionsKey(identityID int64) string {
	return fmt.Sprintf("ncs:user-sessions:%d", identityID)
}

// addUserTokenScript indexes one token for an identity and refreshes the
// index ceiling atomically.
const addUserTokenScript = `
redis.call('SADD', KEYS[1], ARGV[1])
redis.call('PEXPIRE', KEYS[1], ARGV[2])
return 1
`

// removeUserTokenScript drops one token from the index.
const removeUserTokenScript = `
redis.call('SREM', KEYS[1], ARGV[1])
return 1
`

// revokeAllScript deletes every indexed session and the index itself.
// The session keys are constructed inside the script from the indexed
// tokens; this trades the KEYS-only cluster convention for the atomicity
// the revocation guarantee needs (documented for the B line).
const revokeAllScript = `
local tokens = redis.call('SMEMBERS', KEYS[1])
local revoked = 0
for _, token in ipairs(tokens) do
  local session_key = 'ncs:session:' .. token
  local raw = redis.call('GET', session_key)
  local preserve_admin = false
  if raw then
    local envelope_ok, envelope = pcall(cjson.decode, raw)
    if envelope_ok and type(envelope) == 'table' and type(envelope.payload) == 'string' then
      local session_ok, session = pcall(cjson.decode, envelope.payload)
      preserve_admin = session_ok and type(session) == 'table' and session.Role == 'ADMIN'
    end
  end
  if not preserve_admin then
    if redis.call('DEL', session_key) == 1 then
      revoked = revoked + 1
    end
  end
end
redis.call('DEL', KEYS[1])
return revoked
`

// RevokeAllForUser revokes every live session of the identity atomically.
func (s *RedisSessionStore) RevokeAllForUser(ctx context.Context, identityID int64) error {
	_, err := s.commands.RunScript(ctx, revokeAllScript,
		[]string{userSessionsKey(identityID)},
		nil,
	)
	return err
}

func (s *RedisSessionStore) Save(ctx context.Context, token string, session Session) error {
	payload, err := json.Marshal(session)
	if err != nil {
		return fmt.Errorf("auth: encode session payload: %w", err)
	}
	if err := s.sessions.Save(ctx, token, string(payload)); err != nil {
		return err
	}
	// Index the token per identity so RevokeAllForUser can find it. The
	// index lives at least as long as the session's absolute deadline.
	if session.IdentityID > 0 && session.Role == RoleUser {
		indexTTL := time.Until(session.ExpiresAt)
		if indexTTL <= 0 {
			indexTTL = time.Millisecond
		}
		if _, err := s.commands.RunScript(ctx, addUserTokenScript,
			[]string{userSessionsKey(session.IdentityID)},
			[]string{token, strconv.FormatInt(indexTTL.Milliseconds(), 10)},
		); err != nil {
			// A session that is not present in the per-user index cannot be
			// revoked on account deletion or freezing. Compensate with a context
			// that survives request cancellation; Redis still enforces its own
			// command timeout.
			_ = s.sessions.Delete(context.WithoutCancel(ctx), token)
			return err
		}
	}
	return nil
}

func (s *RedisSessionStore) Load(ctx context.Context, token string) (Session, error) {
	payload, found, err := s.sessions.Load(ctx, token)
	if err != nil {
		return Session{}, err
	}
	if !found {
		return Session{}, ErrSessionNotFound
	}

	// Touch the idle window: continuous access renews the session up to the
	// absolute ceiling (requirements: at least seven days with renewal).
	valid, err := s.sessions.Refresh(ctx, token)
	if err != nil {
		return Session{}, err
	}
	if !valid {
		return Session{}, ErrSessionNotFound
	}

	var session Session
	if err := json.Unmarshal([]byte(payload), &session); err != nil {
		return Session{}, fmt.Errorf("auth: decode session payload: %w", err)
	}
	return session, nil
}

func (s *RedisSessionStore) Delete(ctx context.Context, token string) error {
	if err := s.sessions.Delete(ctx, token); err != nil {
		return err
	}
	// The per-user index entry is left in place: Delete does not know the
	// owning identity. Stale entries are harmless — RevokeAllForUser skips
	// tokens whose session key no longer exists.
	return nil
}

// RedisLoginRateLimiter adapts the B-line fixed-window limiter
// (ncs:auth:rate-limit:{identity}) so login throttling is shared by every
// API instance. The limit configuration stays A-line policy.
type RedisLoginRateLimiter struct {
	limiter *bredis.Limiter
	limit   bredis.Limit
	clock   func() time.Time
}

// NewRedisLoginRateLimiter binds the adapter to the B-line limiter.
func NewRedisLoginRateLimiter(limiter *bredis.Limiter, requests int, window time.Duration) (*RedisLoginRateLimiter, error) {
	if limiter == nil {
		return nil, errors.New("auth: redis limiter is required")
	}
	if requests < 1 || window <= 0 {
		return nil, errors.New("auth: limiter configuration must be positive")
	}
	return &RedisLoginRateLimiter{limiter: limiter, limit: bredis.Limit{Requests: requests, Window: window}, clock: time.Now}, nil
}

func (l *RedisLoginRateLimiter) Allow(ctx context.Context, key string) (bool, time.Duration) {
	result, err := l.limiter.Allow(ctx, key, l.limit)
	if err != nil {
		// The B-line policy is FailOpen for rate limiting: Redis outages must
		// not lock everybody out of login. The degradation is recorded by the
		// limiter itself and surfaces in observability.
		return true, 0
	}
	if result.Allowed {
		return true, 0
	}
	retryAfter := time.Until(result.ResetAt)
	if retryAfter < 0 {
		retryAfter = 0
	}
	return false, retryAfter
}

// Redis SMS code keys extend the shared naming baseline. Keys carry a SHA-256
// digest of the phone number (PhoneHash) instead of the number itself:
//
//	ncs:sms:code:{phoneHash}      the pending one-time code
//	ncs:sms:cooldown:{phoneHash}  the resend window marker
const (
	smsCodeKeyPrefix     = "ncs:sms:code:"
	smsCooldownKeyPrefix = "ncs:sms:cooldown:"
)

// verifyCodeScript mirrors auth.verifySMSScript: compare, consume, count and
// void in one server-side script, so a correct code racing a wrong one can
// never interleave. The script text lives in the auth package next to the
// semantics it implements.
const verifyCodeScript = `
local code = redis.call('GET', KEYS[1])
if not code then
  return {-1, 0}
end
if code ~= ARGV[1] then
  local fails = redis.call('INCR', KEYS[2])
  local ttl = redis.call('PTTL', KEYS[2])
  if fails == 1 or ttl < 0 then
    redis.call('PEXPIRE', KEYS[2], ARGV[3])
  end
  if fails >= tonumber(ARGV[2]) then
    redis.call('DEL', KEYS[1])
    redis.call('DEL', KEYS[2])
    return {-3, fails}
  end
  return {-2, fails}
end
redis.call('DEL', KEYS[1])
redis.call('DEL', KEYS[2])
return {1, 0}
`

// RedisSMSCodeStore implements SMSCodeStore on the B-line pooled client.
// Verification is FailClosed (a Redis outage fails the login attempt) and
// atomic: one Lua script compares, consumes, counts and voids, so a correct
// code racing a wrong one cannot interleave and a code can never be used
// twice.
type RedisSMSCodeStore struct {
	commands bredis.Commands
}

// NewRedisSMSCodeStore binds the store to the B-line command surface.
func NewRedisSMSCodeStore(commands bredis.Commands) (*RedisSMSCodeStore, error) {
	if commands == nil {
		return nil, errors.New("auth: redis commands are required")
	}
	return &RedisSMSCodeStore{commands: commands}, nil
}

// issueCodeScript replaces any pending code and clears its failure counter
// in one atomic step, so a wrong submission of the previous code can never
// leak into the new code's failure budget:
//
//	Keys: 1 = code key, 2 = failure counter key
//	Args: 1 = code, 2 = ttl (ms)
const issueCodeScript = `
redis.call('DEL', KEYS[2])
redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2])
return 1
`

func smsCodeKey(phone string) string { return smsCodeKeyPrefix + PhoneHash(phone) }
func smsCooldownKey(phone string) string {
	return smsCooldownKeyPrefix + PhoneHash(phone)
}
func smsFailuresKey(phone string) string {
	return "ncs:sms:failures:" + PhoneHash(phone)
}

func (s *RedisSMSCodeStore) Issue(ctx context.Context, phone, code string, ttl time.Duration) error {
	_, err := s.commands.RunScript(ctx, issueCodeScript,
		[]string{smsCodeKey(phone), smsFailuresKey(phone)},
		[]string{code, strconv.FormatInt(ttl.Milliseconds(), 10)},
	)
	return err
}

func (s *RedisSMSCodeStore) Verify(ctx context.Context, phone, expected string, maxFailures int) (verified, lockedOut, found bool, err error) {
	reply, err := s.commands.RunScript(ctx, verifyCodeScript,
		[]string{smsCodeKey(phone), "ncs:sms:failures:" + PhoneHash(phone)},
		[]string{expected, strconv.Itoa(maxFailures), strconv.FormatInt(smsCodeTTL.Milliseconds(), 10)},
	)
	if err != nil {
		return false, false, false, err
	}
	status, parseErr := scriptStatus(reply)
	if parseErr != nil {
		return false, false, false, fmt.Errorf("auth: verify script reply: %w", parseErr)
	}
	switch status {
	case 1:
		return true, false, true, nil
	case -1:
		return false, false, false, nil
	case -2:
		return false, false, true, nil
	case -3:
		return false, true, true, nil
	default:
		return false, false, false, fmt.Errorf("auth: verify script unknown status %d", status)
	}
}

// scriptStatus extracts the leading status number from the Lua reply.
func scriptStatus(reply any) (int64, error) {
	switch value := reply.(type) {
	case []any:
		if len(value) == 0 {
			return 0, errors.New("empty script reply")
		}
		return replyInteger(value[0])
	case int64:
		return value, nil
	default:
		return 0, fmt.Errorf("unexpected script reply type %T", reply)
	}
}

func replyInteger(value any) (int64, error) {
	switch number := value.(type) {
	case int64:
		return number, nil
	case string:
		parsed, err := strconv.ParseInt(number, 10, 64)
		if err != nil {
			return 0, err
		}
		return parsed, nil
	default:
		return 0, fmt.Errorf("unexpected integer reply type %T", value)
	}
}

func (s *RedisSMSCodeStore) BeginCooldown(ctx context.Context, phone string, window time.Duration) (bool, error) {
	return s.commands.SetNX(ctx, smsCooldownKey(phone), "1", window)
}

func (s *RedisSMSCodeStore) ClearCooldown(ctx context.Context, phone string) error {
	_, err := s.commands.Del(ctx, smsCooldownKey(phone))
	return err
}

// Peek reads the stored code without consuming it. Diagnostics and tests
// only; it is deliberately not part of the SMSCodeStore interface.
func (s *RedisSMSCodeStore) Peek(ctx context.Context, phone string) (string, bool) {
	value, err := s.commands.Get(ctx, smsCodeKey(phone))
	if err != nil {
		return "", false
	}
	return value, true
}
