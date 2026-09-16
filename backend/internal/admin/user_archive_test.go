package admin

import (
	"errors"
	"fmt"
	"net/http"
	"strings"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

// Manual user archiving: what the endpoints accept, what they refuse before
// opening a transaction, and what the console receives.

const (
	usersPath      = "/api/v1/admin/users"
	usersBatchPath = UserBatchPath
)

func userBody(phone, displayName string) string {
	if displayName == "" {
		return `{"phone":"` + phone + `"}`
	}
	return `{"phone":"` + phone + `","displayName":"` + displayName + `"}`
}

func rosterBody(users string) string {
	return `{"users":[` + users + `]}`
}

func oneUser(phone string) string {
	return `{"phone":"` + phone + `"}`
}

// —— authorization ——

// Archiving an account is a write, and it needs an idempotency key like every
// other write on this boundary.
func TestCreateUserRequiresAWriterWithAKey(t *testing.T) {
	body := userBody("13912340000", "")

	anonymous := newFixture(t, auth.Identity{}, false)
	if recorder, _ := do(t, anonymous.server.Handler(), http.MethodPost, usersPath, body,
		map[string]string{"Idempotency-Key": idemKey}); recorder.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401 without a session", recorder.Code)
	}

	auditor := newFixture(t, adminIdentity(auth.AdminRoleAuditor), true)
	recorder, payload := do(t, auditor.server.Handler(), http.MethodPost, usersPath, body,
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusForbidden || payload["code"] != float64(httpapi.CodeForbidden) {
		t.Fatalf("auditor: status = %d code = %v", recorder.Code, payload["code"])
	}
	if len(auditor.store.userCommands) != 0 {
		t.Fatal("a refused draft reached the store")
	}

	operator := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	recorder, _ = do(t, operator.server.Handler(), http.MethodPost, usersPath, body, nil)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 without an idempotency key", recorder.Code)
	}
	if len(operator.store.userCommands) != 0 {
		t.Fatal("a request without a key reached the store")
	}
}

func TestCreateUsersRequiresAWriterWithAKey(t *testing.T) {
	body := rosterBody(oneUser("13912340001"))

	auditor := newFixture(t, adminIdentity(auth.AdminRoleAuditor), true)
	recorder, _ := do(t, auditor.server.Handler(), http.MethodPost, usersBatchPath, body,
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusForbidden {
		t.Fatalf("auditor batch: status = %d", recorder.Code)
	}
	if len(auditor.store.userBatchCommands) != 0 {
		t.Fatal("a refused batch reached the store")
	}

	operator := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	recorder, _ = do(t, operator.server.Handler(), http.MethodPost, usersBatchPath, body, nil)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 without an idempotency key", recorder.Code)
	}
	if len(operator.store.userBatchCommands) != 0 {
		t.Fatal("a request without a key reached the store")
	}
}

// —— validation ——

// The phone is the account's identity and the door every other entry uses; a
// draft that cannot sign in cannot be archived.
func TestCreateUserRefusesAnUnusablePhone(t *testing.T) {
	operator := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	unusable := []string{
		"1391234000",   // nine digits after the prefix
		"139123400001", // eleven digits after the prefix
		"23912340000",  // a prefix no Chinese carrier signs in with
		"1391234000a",  // a letter where a digit belongs
		"",             // nothing at all
	}
	for _, phone := range unusable {
		recorder, _ := do(t, operator.server.Handler(), http.MethodPost, usersPath, userBody(phone, ""),
			map[string]string{"Idempotency-Key": idemKey})
		if recorder.Code != http.StatusBadRequest {
			t.Errorf("phone %q: status = %d, want 400", phone, recorder.Code)
		}
	}
	if len(operator.store.userCommands) != 0 {
		t.Fatal("an unusable draft reached the store")
	}

	// A display name longer than the profile contract's bound is refused with
	// the same answer.
	long := strings.Repeat("名", maxDisplayNameRunes+1)
	recorder, _ := do(t, operator.server.Handler(), http.MethodPost, usersPath, userBody("13912340000", long),
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("long display name: status = %d", recorder.Code)
	}
}

func TestCreateUsersRefusesABrokenRoster(t *testing.T) {
	operator := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)

	// A phone repeated inside one page names the phone, before any transaction.
	repeated := rosterBody(oneUser("13912340000") + "," + oneUser("13912340000"))
	recorder, payload := do(t, operator.server.Handler(), http.MethodPost, usersBatchPath, repeated,
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusConflict || payload["code"] != float64(codeAlreadyExists) {
		t.Fatalf("repeated phone: status = %d code = %v", recorder.Code, payload["code"])
	}
	if len(operator.store.userBatchCommands) != 0 {
		t.Fatal("a repeated phone reached the store")
	}

	// One unusable entry refuses the page, and the answer names the entry.
	recorder, _ = do(t, operator.server.Handler(), http.MethodPost, usersBatchPath,
		rosterBody(oneUser("13912340000")+","+oneUser("bad")),
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("unusable entry: status = %d", recorder.Code)
	}

	// An empty page is a malformed request, not a no-op.
	recorder, _ = do(t, operator.server.Handler(), http.MethodPost, usersBatchPath, rosterBody(""),
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("empty batch: status = %d", recorder.Code)
	}

	// An oversized page is refused by the service, before the store.
	oversized := `{"users":[`
	for index := 0; index < MaxUserBatch+1; index++ {
		if index > 0 {
			oversized += ","
		}
		oversized += oneUser(fmt.Sprintf("139%08d", index))
	}
	oversized += `]}`
	recorder, _ = do(t, operator.server.Handler(), http.MethodPost, usersBatchPath, oversized,
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("oversized batch: status = %d", recorder.Code)
	}
	if len(operator.store.userBatchCommands) != 0 {
		t.Fatal("a refused page reached the store")
	}
}

// —— the store boundary ——

func TestCreateUserReturnsWhatItCreated(t *testing.T) {
	operator := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	operator.store.userResult = UserRecord{ID: 31, Phone: "13912340000", DisplayName: "老王", Status: UserStatusActive}

	recorder, payload := do(t, operator.server.Handler(), http.MethodPost, usersPath, userBody("13912340000", "老王"),
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusCreated {
		t.Fatalf("status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	data := payload["data"].(map[string]any)
	if data["id"] != float64(31) || data["phone"] != "13912340000" || data["status"] != UserStatusActive {
		t.Fatalf("data = %#v", data)
	}

	command := operator.store.userCommands[0]
	if command.User.Phone != "13912340000" || command.User.DisplayName != "老王" {
		t.Fatalf("command = %#v", command)
	}
	if command.IdempotencyKey != idemKey || command.AdminID != 2 {
		t.Fatalf("command meta = %#v", command)
	}

	// GET keeps working on the shared route.
	recorder, _ = do(t, operator.server.Handler(), http.MethodGet, usersPath, "", nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("list status = %d", recorder.Code)
	}
	// PUT is a method error on the shared route, not a silent list.
	recorder, _ = do(t, operator.server.Handler(), http.MethodPut, usersPath, `{}`, nil)
	if recorder.Code != http.StatusMethodNotAllowed {
		t.Fatalf("PUT status = %d", recorder.Code)
	}
}

func TestCreateUsersReturnsWhatItCreated(t *testing.T) {
	operator := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	operator.store.userBatchResult = UserBatchResult{
		UserCount: 2,
		Created: []UserRecord{
			{ID: 31, Phone: "13912340000", DisplayName: "用户0000", Status: UserStatusActive},
			{ID: 32, Phone: "13912340001", DisplayName: "老王", Status: UserStatusActive},
		},
	}

	recorder, payload := do(t, operator.server.Handler(), http.MethodPost, usersBatchPath,
		rosterBody(oneUser("13912340000")+","+`{"phone":"13912340001","displayName":"老王"}`),
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusCreated {
		t.Fatalf("status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	data := payload["data"].(map[string]any)
	if data["userCount"] != float64(2) || len(data["created"].([]any)) != 2 {
		t.Fatalf("data = %#v", data)
	}
	if len(operator.store.userBatchCommands[0].Users) != 2 {
		t.Fatalf("page = %#v", operator.store.userBatchCommands[0])
	}
}

func TestCreateUserReportsAnExistingPhoneAsAConflict(t *testing.T) {
	operator := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	operator.store.userErr = fmt.Errorf("%w: a phone in this request is already registered", ErrDuplicateUserPhone)

	recorder, payload := do(t, operator.server.Handler(), http.MethodPost, usersPath, userBody("13912340000", ""),
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusConflict || payload["code"] != float64(codeAlreadyExists) {
		t.Fatalf("duplicate: status = %d code = %v", recorder.Code, payload["code"])
	}

	operator.store.userErr = errors.New("connection refused")
	recorder, payload = do(t, operator.server.Handler(), http.MethodPost, usersPath, userBody("13912340000", ""),
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusServiceUnavailable || payload["code"] != float64(httpapi.CodeDatabaseError) {
		t.Fatalf("store failure: status = %d code = %v", recorder.Code, payload["code"])
	}
}

// —— the helpers the contract rests on ——

func TestUserArchiveHelpers(t *testing.T) {
	// A masked phone answers "which entry was this" without carrying the full
	// number into the audit trail.
	if masked := MaskPhone("13912340000"); masked != "139****0000" {
		t.Fatalf("masked = %q", masked)
	}
	if masked := MaskPhone("12345"); masked != "***" {
		t.Fatalf("masked short = %q", masked)
	}
	// The default name is the registration default, digit for digit.
	if name := DefaultUserDisplayName("13912340000"); name != "用户0000" {
		t.Fatalf("default name = %q", name)
	}
}
