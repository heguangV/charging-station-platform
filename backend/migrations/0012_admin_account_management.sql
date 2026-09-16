-- Administrator account management needs optimistic concurrency and a first-login marker.
-- Existing accounts are trusted as already initialized; accounts created through the new API
-- start with must_change_password=true.
ALTER TABLE admin_accounts
    ADD COLUMN IF NOT EXISTS must_change_password BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE admin_accounts
    ADD COLUMN IF NOT EXISTS version BIGINT NOT NULL DEFAULT 1 CHECK (version > 0);

CREATE INDEX IF NOT EXISTS idx_admin_accounts_created
    ON admin_accounts (created_at DESC, id DESC);
