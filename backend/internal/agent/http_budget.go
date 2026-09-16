package agent

import (
	"context"
	"net/http"
	"time"
)

// Include authentication and body reading in the budget. Refresh the socket
// deadline at route entry so header parsing cannot consume the reply reserve.
func (h *Handlers) withBudget(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		deadline := time.Now().Add(h.service.budget)
		ctx, cancel := context.WithDeadline(r.Context(), deadline)
		defer cancel()
		controller := http.NewResponseController(w)
		// Recorders and non-network transports may not support deadlines.
		_ = controller.SetReadDeadline(deadline)
		_ = controller.SetWriteDeadline(deadline.Add(time.Second))
		next(w, r.WithContext(ctx))
	}
}
