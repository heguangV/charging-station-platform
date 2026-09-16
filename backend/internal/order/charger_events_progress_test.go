package order

import (
	"net/http"
	"strconv"
	"testing"
	"time"
)

func progressReceipt(occurredAt time.Time, energyWh int64) string {
	return `{"eventId":"evt_receipt_progress","eventType":"CHARGE_PROGRESS","orderNo":"ORD20260914120000aaaa",` +
		`"chargerId":42,"occurredAt":"` + occurredAt.UTC().Format(time.RFC3339Nano) + `",` +
		`"energyWh":` + strconv.FormatInt(energyWh, 10) + `,"traceId":"trace-gw"}`
}

// The running meter is what lets the app show an amount during a charge; the
// receipt reaches the order service as a progress confirmation, not as a
// transition.
func TestChargerProgressReceiptReachesTheService(t *testing.T) {
	handler, store := newReceiptFixture(t, 0)
	occurredAt := time.Now().UTC().Add(-time.Minute)

	recorder, payload := do(t, handler, http.MethodPost, ChargerEventPath, progressReceipt(occurredAt, 1200), bearer())
	if recorder.Code != http.StatusOK {
		t.Fatalf("progress status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	if len(store.progress) != 1 {
		t.Fatalf("progress commands = %d, want 1", len(store.progress))
	}
	if store.progress[0].EnergyWh != 1200 || store.progress[0].ChargerID != 42 {
		t.Fatalf("progress command = %#v", store.progress[0])
	}
	if !store.progress[0].OccurredAt.Equal(occurredAt.Truncate(time.Nanosecond)) {
		t.Fatalf("progress fact time = %v, want %v", store.progress[0].OccurredAt, occurredAt)
	}
	if data, ok := payload["data"].(map[string]any); !ok || data["eventType"] != ChargerEventProgress {
		t.Fatalf("response eventType = %v, want %s", payload["data"], ChargerEventProgress)
	}
}

// A reading without energy says nothing about the meter, and a negative one is
// not a reading at all: both are refused before the service sees them.
func TestChargerProgressReceiptValidation(t *testing.T) {
	handler, store := newReceiptFixture(t, 0)
	occurredAt := time.Now().UTC().Add(-time.Minute)

	noEnergy := `{"eventId":"evt_receipt_progress","eventType":"CHARGE_PROGRESS","orderNo":"ORD20260914120000aaaa",` +
		`"chargerId":42,"occurredAt":"` + occurredAt.Format(time.RFC3339Nano) + `","traceId":"trace-gw"}`
	recorder, _ := do(t, handler, http.MethodPost, ChargerEventPath, noEnergy, bearer())
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("missing energyWh status = %d, want 400", recorder.Code)
	}

	recorder, _ = do(t, handler, http.MethodPost, ChargerEventPath, progressReceipt(occurredAt, -1), bearer())
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("negative energyWh status = %d, want 400", recorder.Code)
	}
	if len(store.progress) != 0 {
		t.Fatalf("refused readings reached the service: %#v", store.progress)
	}
}
