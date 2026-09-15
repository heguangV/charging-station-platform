CREATE INDEX ix_order_status_settled ON charging_order(status, settled_at);
CREATE INDEX ix_order_status_started ON charging_order(status, COALESCE(started_at, created_at));
