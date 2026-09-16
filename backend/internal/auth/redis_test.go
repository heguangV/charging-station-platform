package auth

import (
	"context"
	"errors"
	"testing"
	"time"

	bredis "github.com/heguangV/charging-station-platform/backend/internal/repository/redis"
)

type failingScriptCommands struct {
	*bredis.MemoryCommands
	scriptErr error
}

func (c *failingScriptCommands) RunScript(context.Context, string, []string, []string) (any, error) {
	return nil, c.scriptErr
}

func TestRedisSessionStoreRemovesSessionWhenIndexWriteFails(t *testing.T) {
	indexErr := errors.New("index write failed")
	commands := &failingScriptCommands{
		MemoryCommands: bredis.NewMemoryCommands(),
		scriptErr:      indexErr,
	}
	sessions, err := bredis.NewSessions(
		commands,
		bredis.DefaultPolicy(),
		nil,
		bredis.SessionConfig{IdleTTL: time.Minute, AbsoluteTTL: time.Hour},
		nil,
	)
	if err != nil {
		t.Fatalf("NewSessions() error = %v", err)
	}
	store, err := NewRedisSessionStore(sessions, commands)
	if err != nil {
		t.Fatalf("NewRedisSessionStore() error = %v", err)
	}

	err = store.Save(context.Background(), "token-1", Session{
		IdentityID: 7,
		Role:       RoleUser,
		ExpiresAt:  time.Now().Add(time.Hour),
	})
	if !errors.Is(err, indexErr) {
		t.Fatalf("Save() error = %v, want %v", err, indexErr)
	}
	if _, found, err := sessions.Load(context.Background(), "token-1"); err != nil {
		t.Fatalf("Load() after compensation error = %v", err)
	} else if found {
		t.Fatal("session survived a failed user-index write")
	}
}

func TestRedisSessionStoreDoesNotIndexAdminAsUser(t *testing.T) {
	commands := &failingScriptCommands{
		MemoryCommands: bredis.NewMemoryCommands(),
		scriptErr:      errors.New("user index must not be written"),
	}
	sessions, err := bredis.NewSessions(
		commands,
		bredis.DefaultPolicy(),
		nil,
		bredis.SessionConfig{IdleTTL: time.Minute, AbsoluteTTL: time.Hour},
		nil,
	)
	if err != nil {
		t.Fatalf("NewSessions() error = %v", err)
	}
	store, err := NewRedisSessionStore(sessions, commands)
	if err != nil {
		t.Fatalf("NewRedisSessionStore() error = %v", err)
	}

	err = store.Save(context.Background(), "admin-token", Session{
		IdentityID: 7,
		Role:       RoleAdmin,
		ExpiresAt:  time.Now().Add(time.Hour),
	})
	if err != nil {
		t.Fatalf("Save(admin) error = %v", err)
	}
	if _, found, err := sessions.Load(context.Background(), "admin-token"); err != nil {
		t.Fatalf("Load(admin) error = %v", err)
	} else if !found {
		t.Fatal("admin session was not saved")
	}
}
