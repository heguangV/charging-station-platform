-- A-04 / B-02 follow-up: appeals need a rejection outcome.
--
-- The queue could only be emptied by approving, and approving cancels the order and refunds the
-- settled amount. A spurious appeal therefore had no way out: the operator had to refund to close
-- it. Rejecting records the decision, leaves the order and the wallet untouched, and keeps the
-- operator's reason so the customer can read why the appeal was dismissed.
--
-- The 0009 table declared the status inline, which pinned it to PENDING/APPROVED; the constraint
-- has to be replaced rather than extended.
ALTER TABLE order_appeals DROP CONSTRAINT IF EXISTS order_appeals_status_check;
ALTER TABLE order_appeals
    ADD CONSTRAINT order_appeals_status_check
        CHECK (status IN ('PENDING', 'APPROVED', 'REJECTED'));
ALTER TABLE order_appeals
    ADD COLUMN IF NOT EXISTS decision_reason TEXT
        CHECK (decision_reason IS NULL OR length(decision_reason) BETWEEN 1 AND 500);

-- The admin queue filters on status and orders by creation time.
DROP INDEX IF EXISTS idx_order_appeals_status_created;
CREATE INDEX IF NOT EXISTS idx_order_appeals_status_created
    ON order_appeals (status, created_at DESC);
