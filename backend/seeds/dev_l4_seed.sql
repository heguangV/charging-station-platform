-- L4 (high-capacity platform scale) development seed. NEVER apply to
-- staging or production.
--
-- Profile "开发 L4 / 高容量平台级":
--   registered users   : 10,000,000  (phone range 13800000000..13899999999)
--   charging stations  : 10,000      (codes L4S00001..L4S10000)
--   charging devices   : 100,000     (10 per station: 4 DC + 6 AC)
-- Every number can be overridden per run, e.g. a smaller smoke profile:
--   psql "$DSN" -v l4_users=10000 -v l4_stations=20 -f backend/seeds/dev_l4_seed.sql
--
-- Apply after migrations (and ideally after seeds/dev_seed.sql):
--   psql "<development dsn>" -f backend/seeds/dev_l4_seed.sql
--
-- Safe to re-run: every insert is guarded by its natural key
-- (unique phone / station code / (station_id, code) via ON CONFLICT DO
-- NOTHING), so an interrupted run simply continues.
--
-- The synthetic accounts share the dev-only fixed password hash from
-- seeds/dev_seed.sql and never leave development. Wallet balances are
-- deterministic (every 20th account stays at zero to exercise debt paths).
--
-- Full L4 duration on a laptop-class machine: roughly 2-6 minutes, most of
-- it in the two 10M-row inserts and their constraints.

\set ON_ERROR_STOP on

-- Allow smaller profiles without editing the file.
\if :{?l4_users}
\else
\set l4_users 10000000
\endif
\if :{?l4_stations}
\else
\set l4_stations 10000
\endif

-- Disposable data: skip per-statement fsync for the duration of this session.
SET synchronous_commit = off;

-- 10M users, created_at spread over ~800 days for realistic ordering.
INSERT INTO user_accounts (phone, display_name, password_hash, status, created_at, updated_at)
SELECT '138' || lpad(g::text, 8, '0'),
       'L4用户' || lpad(g::text, 8, '0'),
       'pbkdf2-sha256$210000$ZGV2LXNlZWQtc2FsdC0wMDAx$WK4g4MFTwN9ZgPXG88KwLjbCqOj0wzVQdyosT69m414',
       'ACTIVE',
       CURRENT_TIMESTAMP - ((g % 69120000)::text || ' seconds')::interval,
       CURRENT_TIMESTAMP
FROM generate_series(1, :'l4_users'::bigint) AS g
ON CONFLICT (phone) DO NOTHING;

-- Wallets for the synthetic accounts only (phone prefix 138 + 8 digits).
-- Deterministic balances 500..48500 cents; every 20th account stays at zero
-- so the insufficient-balance and debt paths stay exercisable. Existing
-- wallets (e.g. the dev_seed user) are never touched.
INSERT INTO wallet_accounts (user_id, balance_cents, created_at, updated_at)
SELECT u.id,
       CASE WHEN u.id % 20 = 0 THEN 0 ELSE ((u.id % 97) + 1) * 500 END,
       u.created_at,
       u.created_at
FROM user_accounts u
WHERE u.phone ~ '^138[0-9]{8}$'
ON CONFLICT (user_id) DO NOTHING;

-- 10k stations spread over mainland China coordinates.
INSERT INTO stations (code, name, address, latitude, longitude, status, created_at, updated_at)
SELECT 'L4S' || lpad(g::text, 5, '0'),
       'L4压测站' || lpad(g::text, 5, '0'),
       'L4压测路' || (((g % 9999) + 1)::text) || '号',
       round((18000000 + ((g::bigint * 7919) % 20000000))::numeric / 1000000, 6),
       round((73000000 + ((g::bigint * 104729) % 62000000))::numeric / 1000000, 6),
       CASE WHEN g % 500 = 0 THEN 'CLOSED' ELSE 'OPEN' END,
       CURRENT_TIMESTAMP - ((g % 51840000)::text || ' seconds')::interval,
       CURRENT_TIMESTAMP
FROM generate_series(1, :'l4_stations'::bigint) AS g
ON CONFLICT (code) DO NOTHING;

-- 10 devices per L4 station (4 DC 120 kW + 6 AC 7 kW); ~1% FAULT so the
-- admin fault workflows have data.
INSERT INTO chargers (station_id, code, connector_type, power_watt, status,
                      price_per_kwh_cents, service_price_per_kwh_cents,
                      created_at, updated_at)
SELECT s.id,
       s.code || '-' || CASE WHEN c.j <= 4 THEN 'DC' ELSE 'AC' END || '-' || lpad(c.j::text, 2, '0'),
       CASE WHEN c.j <= 4 THEN 'DC' ELSE 'AC' END,
       CASE WHEN c.j <= 4 THEN 120000 ELSE 7000 END,
       CASE WHEN (s.id * 10 + c.j) % 97 = 0 THEN 'FAULT' ELSE 'IDLE' END,
       CASE WHEN c.j <= 4 THEN 120 ELSE 100 END,
       CASE WHEN c.j <= 4 THEN 50 ELSE 40 END,
       s.created_at,
       s.created_at
FROM stations s
CROSS JOIN generate_series(1, 10) AS c(j)
WHERE s.code ~ '^L4S[0-9]{5}$'
ON CONFLICT DO NOTHING;

RESET synchronous_commit;

-- Planner statistics for the fresh rows keep the API queries sane.
ANALYZE user_accounts;
ANALYZE wallet_accounts;
ANALYZE stations;
ANALYZE chargers;

\echo 'L4 seed summary:'
SELECT (SELECT count(*) FROM user_accounts WHERE phone ~ '^138[0-9]{8}$')          AS l4_users,
       (SELECT count(*) FROM wallet_accounts w
          JOIN user_accounts u ON u.id = w.user_id WHERE u.phone ~ '^138[0-9]{8}$') AS l4_wallets,
       (SELECT count(*) FROM stations WHERE code LIKE 'L4S%')                        AS l4_stations,
       (SELECT count(*) FROM chargers c
          JOIN stations s ON s.id = c.station_id WHERE s.code LIKE 'L4S%')           AS l4_chargers;
