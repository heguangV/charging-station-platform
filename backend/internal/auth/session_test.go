package auth

import (
	"context"
	"errors"
	"sync"
	"testing"
	"time"
)

func TestInMemoryStoreSaveLoadDelete(t *testing.T) {
	store := NewInMemorySessionStore(time.Minute, nil)
	ctx := context.Background()

	session := Session{IdentityID: 7, Role: RoleUser, DisplayName: "开发用户", ExpiresAt: time.Now().Add(time.Hour)}
	if err := store.Save(ctx, "token-1", session); err != nil {
		t.Fatalf("Save() error = %v", err)
	}

	loaded, err := store.Load(ctx, "token-1")
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	if loaded.IdentityID != 7 || loaded.Role != RoleUser || loaded.DisplayName != "开发用户" {
		t.Fatalf("loaded session = %#v", loaded)
	}

	if err := store.Delete(ctx, "token-1"); err != nil {
		t.Fatalf("Delete() error = %v", err)
	}
	if _, err := store.Load(ctx, "token-1"); err != nil && err.Error() != ErrSessionNotFound.Error() {
		t.Fatalf("Load() after delete error = %v", err)
	} else if err == nil {
		t.Fatal("Load() after delete returned a session")
	}

	// Deleting an unknown token stays a no-op.
	if err := store.Delete(ctx, "token-unknown"); err != nil {
		t.Fatalf("Delete(unknown) error = %v", err)
	}
}

func TestInMemoryStoreAbsoluteExpiry(t *testing.T) {
	now := time.Now()
	clock := func() time.Time { return now }
	store := NewInMemorySessionStore(time.Minute, clock)
	ctx := context.Background()

	if err := store.Save(ctx, "token-1", Session{IdentityID: 1, Role: RoleUser, ExpiresAt: now.Add(time.Hour)}); err != nil {
		t.Fatalf("Save() error = %v", err)
	}

	now = now.Add(2 * time.Hour) // past the absolute deadline
	if _, err := store.Load(ctx, "token-1"); err != nil && err.Error() != ErrSessionNotFound.Error() {
		t.Fatalf("expired Load error = %v", err)
	} else if err == nil {
		t.Fatal("expired session still loads")
	}
}

func TestInMemoryStoreIdleExpiryAndTouch(t *testing.T) {
	now := time.Now()
	clock := func() time.Time { return now }
	store := NewInMemorySessionStore(10*time.Minute, clock)
	ctx := context.Background()

	if err := store.Save(ctx, "token-1", Session{IdentityID: 1, Role: RoleUser, ExpiresAt: now.Add(time.Hour)}); err != nil {
		t.Fatalf("Save() error = %v", err)
	}

	now = now.Add(5 * time.Minute)
	if _, err := store.Load(ctx, "token-1"); err != nil {
		t.Fatalf("Load() within idle window error = %v", err)
	}

	// The load touched the cursor: another 5 idle minutes are fine.
	now = now.Add(5 * time.Minute)
	if _, err := store.Load(ctx, "token-1"); err != nil {
		t.Fatalf("Load() after touch error = %v", err)
	}

	// Past the touched idle window the session expires even though the
	// absolute deadline is far away.
	now = now.Add(11 * time.Minute)
	if _, err := store.Load(ctx, "token-1"); err == nil {
		t.Fatal("idle-expired session still loads")
	}
	if _, indexed := store.byUser[1]; indexed {
		t.Fatal("idle-expired session remained in the user index")
	}
}

func TestInMemoryStoreExpiresAtIdleBoundary(t *testing.T) {
	now := time.Now()
	clock := func() time.Time { return now }
	store := NewInMemorySessionStore(10*time.Minute, clock)
	ctx := context.Background()

	if err := store.Save(ctx, "token-1", Session{IdentityID: 1, Role: RoleUser, ExpiresAt: now.Add(time.Hour)}); err != nil {
		t.Fatalf("Save() error = %v", err)
	}
	now = now.Add(10 * time.Minute)
	if _, err := store.Load(ctx, "token-1"); !errors.Is(err, ErrSessionNotFound) {
		t.Fatalf("Load() at idle boundary error = %v, want ErrSessionNotFound", err)
	}
}

func TestInMemoryStoreUserRevocationDoesNotRevokeAdminWithSameID(t *testing.T) {
	store := NewInMemorySessionStore(time.Minute, nil)
	ctx := context.Background()
	expiresAt := time.Now().Add(time.Hour)

	if err := store.Save(ctx, "user-token", Session{IdentityID: 7, Role: RoleUser, ExpiresAt: expiresAt}); err != nil {
		t.Fatalf("Save(user) error = %v", err)
	}
	if err := store.Save(ctx, "admin-token", Session{IdentityID: 7, Role: RoleAdmin, ExpiresAt: expiresAt}); err != nil {
		t.Fatalf("Save(admin) error = %v", err)
	}
	if err := store.RevokeAllForUser(ctx, 7); err != nil {
		t.Fatalf("RevokeAllForUser() error = %v", err)
	}
	if _, err := store.Load(ctx, "user-token"); !errors.Is(err, ErrSessionNotFound) {
		t.Fatalf("Load(user) after revocation error = %v, want ErrSessionNotFound", err)
	}
	if _, err := store.Load(ctx, "admin-token"); err != nil {
		t.Fatalf("Load(admin) after user revocation error = %v", err)
	}
}

func TestInMemoryStoreOverwriteMovesUserIndex(t *testing.T) {
	store := NewInMemorySessionStore(time.Minute, nil)
	ctx := context.Background()
	expiresAt := time.Now().Add(time.Hour)

	if err := store.Save(ctx, "shared-token", Session{IdentityID: 1, Role: RoleUser, ExpiresAt: expiresAt}); err != nil {
		t.Fatalf("Save(first identity) error = %v", err)
	}
	if err := store.Save(ctx, "shared-token", Session{IdentityID: 2, Role: RoleUser, ExpiresAt: expiresAt}); err != nil {
		t.Fatalf("Save(second identity) error = %v", err)
	}
	if err := store.RevokeAllForUser(ctx, 1); err != nil {
		t.Fatalf("RevokeAllForUser(first identity) error = %v", err)
	}
	loaded, err := store.Load(ctx, "shared-token")
	if err != nil {
		t.Fatalf("token was still indexed to the first identity: %v", err)
	}
	if loaded.IdentityID != 2 {
		t.Fatalf("loaded identity = %d, want 2", loaded.IdentityID)
	}
	if err := store.RevokeAllForUser(ctx, 2); err != nil {
		t.Fatalf("RevokeAllForUser(second identity) error = %v", err)
	}
	if _, err := store.Load(ctx, "shared-token"); !errors.Is(err, ErrSessionNotFound) {
		t.Fatalf("Load() after revoking second identity error = %v", err)
	}
}

func TestInMemoryStoreConcurrentAccess(t *testing.T) {
	store := NewInMemorySessionStore(time.Minute, nil)
	ctx := context.Background()

	var group sync.WaitGroup
	for worker := 0; worker < 8; worker++ {
		group.Add(1)
		go func(worker int) {
			defer group.Done()
			token := "token-" + time.Duration(worker).String()
			for round := 0; round < 50; round++ {
				_ = store.Save(ctx, token, Session{IdentityID: int64(worker), ExpiresAt: time.Now().Add(time.Minute)})
				if _, err := store.Load(ctx, token); err != nil {
					t.Errorf("Load() error = %v", err)
					return
				}
				_ = store.Delete(ctx, token)
			}
		}(worker)
	}
	group.Wait()
}

func TestInMemoryStoreRejectsEmptyToken(t *testing.T) {
	store := NewInMemorySessionStore(time.Minute, nil)
	if err := store.Save(context.Background(), "", Session{IdentityID: 1}); err == nil {
		t.Fatal("empty token accepted")
	}
}
