-- Development seed data for the P0 schema. NEVER apply to staging or production.
-- Apply after migrations with:
--   psql "<development dsn>" -f backend/seeds/dev_seed.sql
-- Seeded credentials (development only): password Dev-Password-01 for
--   admin  / admin_accounts (SUPER_ADMIN)
--   13800000001 / user_accounts
-- password_hash format: pbkdf2-sha256$iterations$salt$derived_key (see
-- backend/internal/auth/password.go); the fixed salt is acceptable because
-- this file must never leave development.
-- The file is safe to re-run: every insert is guarded by its natural key.

INSERT INTO admin_accounts (username, password_hash, role)
VALUES ('admin', 'pbkdf2-sha256$210000$ZGV2LXNlZWQtc2FsdC0wMDAx$WK4g4MFTwN9ZgPXG88KwLjbCqOj0wzVQdyosT69m414', 'SUPER_ADMIN')
ON CONFLICT DO NOTHING;

INSERT INTO user_accounts (phone, display_name, password_hash)
VALUES ('13800000001', '开发用户', 'pbkdf2-sha256$210000$ZGV2LXNlZWQtc2FsdC0wMDAx$WK4g4MFTwN9ZgPXG88KwLjbCqOj0wzVQdyosT69m414')
ON CONFLICT DO NOTHING;

-- Wallet starts with 50.00 so the BR-04 minimum start balance (500 cents)
-- passes and the P0 order loop is usable in development. Production wallets
-- are created at registration with a zero balance.
INSERT INTO wallet_accounts (user_id, balance_cents)
SELECT id, 5000 FROM user_accounts WHERE phone = '13800000001'
ON CONFLICT DO NOTHING;

INSERT INTO stations (code, name, address, latitude, longitude, status)
VALUES ('ST-DEV-01', '开发充电站', '成都市高新区天府软件园', 30.545200, 104.070800, 'OPEN')
ON CONFLICT DO NOTHING;

-- price_per_kwh_cents is the electricity fee, service_price the service fee
-- (BR-05); A01 carries a time-of-use off-peak window (23:00-07:00).
INSERT INTO chargers (station_id, code, connector_type, power_watt, status,
                      price_per_kwh_cents, service_price_per_kwh_cents,
                      off_peak_electricity_price_per_kwh_cents, off_peak_start_hour, off_peak_end_hour)
SELECT s.id, c.code, c.connector_type, c.power_watt, 'IDLE',
       c.price_per_kwh_cents, c.service_price_per_kwh_cents,
       c.off_peak, c.peak_start, c.peak_end
FROM stations s
JOIN (VALUES
    ('A01', 'AC', 7000::BIGINT, 100, 50, 60::INTEGER, 23::SMALLINT, 7::SMALLINT),
    ('B01', 'DC', 120000::BIGINT, 120, 50, NULL::INTEGER, NULL::SMALLINT, NULL::SMALLINT)
) AS c(code, connector_type, power_watt, price_per_kwh_cents, service_price_per_kwh_cents, off_peak, peak_start, peak_end) ON TRUE
WHERE s.code = 'ST-DEV-01'
ON CONFLICT DO NOTHING;

-- A third charger, in a second station.
--
-- verify-closed-loop.sh drives the order flow and the successful device command on the two chargers
-- of ST-DEV-01 and takes the failure path on any other idle charger, because a charger held by an
-- active order refuses a restart command. Without a third one the gate cannot run at all, and it
-- refuses with "expected at least three chargers in the database" before it tests anything.
INSERT INTO stations (code, name, address, latitude, longitude, status)
VALUES ('ST-DEV-02', '开发充电站二号', '成都市高新区天府三街', 30.546100, 104.071500, 'OPEN')
ON CONFLICT DO NOTHING;

INSERT INTO chargers (station_id, code, connector_type, power_watt, status,
                      price_per_kwh_cents, service_price_per_kwh_cents)
SELECT s.id, 'C01', 'DC', 60000, 'IDLE', 120, 50
FROM stations s
WHERE s.code = 'ST-DEV-02'
ON CONFLICT DO NOTHING;
