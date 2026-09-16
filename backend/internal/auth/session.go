package auth

import (
	"context"
	"errors"
	"sync"
	"time"
)

// Session is the server-side state behind an opaque bearer token. It carries
// an identity snapshot; PostgreSQL remains the source of truth for accounts.
type Session struct {
	IdentityID   int64
	Role         string // USER or ADMIN, matching the contract Identity.role
	AdminRole    string // SUPER_ADMIN/OPERATOR/AUDITOR for admins
	DisplayName  string
	Status       string    // account status snapshot, ACTIVE at login time
	ExpiresAt    time.Time // absolute deadline, UTC
	lastAccessed time.Time // idle-TTL cursor, managed by stores
}

// SessionStore persists token-to-session lookups. Implementations must treat
// token as a secret: it is only ever supplied by the API process.
//
// The A line depends on this interface only. The B line owns the Redis
// adapter (ncs:session:{session_id}); the in-memory implementation is the
// development default and keeps tests deterministic.
type SessionStore interface {
	Save(ctx context.Context, token string, session Session) error
	Load(ctx context.Context, token string) (Session, error)
	Delete(ctx context.Context, token string) error
	// RevokeAllForUser revokes every live session of one identity (UC-U-05
	// account deletion, account freezing). Implementations must maintain a
	// per-user index so revocation works without enumerating the token keys.
	RevokeAllForUser(ctx context.Context, identityID int64) error
}

// ErrSessionNotFound is returned by SessionStore.Load for unknown, expired,
// or revoked tokens. Callers must map it to 401 UNAUTHORIZED.
var ErrSessionNotFound = errors.New("auth: session not found")

// InMemorySessionStore is a mutex-guarded map with idle and absolute expiry.
// Expiry mirrors the B-line Redis semantics: access extends the idle window
// but never beyond the absolute deadline.
type InMemorySessionStore struct {
	mu       sync.RWMutex
	sessions map[string]Session
	byUser   map[int64]map[string]struct{}
	idleTTL  time.Duration
	clock    func() time.Time
}

// NewInMemorySessionStore returns a store with the given idle window.
func NewInMemorySessionStore(idleTTL time.Duration, clock func() time.Time) *InMemorySessionStore {
	if idleTTL <= 0 {
		idleTTL = 30 * time.Minute
	}
	if clock == nil {
		clock = time.Now
	}
	return &InMemorySessionStore{
		sessions: make(map[string]Session),
		byUser:   make(map[int64]map[string]struct{}),
		idleTTL:  idleTTL,
		clock:    clock,
	}
}

// Save stores a session under the token, stamping its idle cursor.
func (s *InMemorySessionStore) Save(_ context.Context, token string, session Session) error {
	if token == "" {
		return errors.New("auth: session token is empty")
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	if _, exists := s.sessions[token]; exists {
		s.deleteLocked(token)
	}
	session.lastAccessed = s.clock()
	s.sessions[token] = session
	if session.IdentityID > 0 && session.Role == RoleUser {
		if s.byUser[session.IdentityID] == nil {
			s.byUser[session.IdentityID] = make(map[string]struct{})
		}
		s.byUser[session.IdentityID][token] = struct{}{}
	}
	return nil
}

// Load returns the session if the token is live. A live session has not
// passed its absolute deadline and was accessed within the idle window.
// Loading touches the idle cursor.
func (s *InMemorySessionStore) Load(_ context.Context, token string) (Session, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	session, ok := s.sessions[token]
	if !ok {
		return Session{}, ErrSessionNotFound
	}
	now := s.clock()
	if !now.Before(session.ExpiresAt) || now.Sub(session.lastAccessed) >= s.idleTTL {
		s.deleteLocked(token)
		return Session{}, ErrSessionNotFound
	}
	session.lastAccessed = now
	s.sessions[token] = session
	return session, nil
}

// Delete removes the session. Deleting an unknown token is not an error so
// logout stays idempotent.
func (s *InMemorySessionStore) Delete(_ context.Context, token string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.deleteLocked(token)
	return nil
}

// deleteLocked removes a session and its reverse-index entry. The caller must
// hold s.mu for writing.
func (s *InMemorySessionStore) deleteLocked(token string) {
	if session, ok := s.sessions[token]; ok && session.IdentityID > 0 && session.Role == RoleUser {
		if tokens := s.byUser[session.IdentityID]; tokens != nil {
			delete(tokens, token)
			if len(tokens) == 0 {
				delete(s.byUser, session.IdentityID)
			}
		}
	}
	delete(s.sessions, token)
}

// RevokeAllForUser revokes every live session of the identity.
func (s *InMemorySessionStore) RevokeAllForUser(_ context.Context, identityID int64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	for token := range s.byUser[identityID] {
		delete(s.sessions, token)
	}
	delete(s.byUser, identityID)
	return nil
}
