-- Charging needs a running meter, not just a final one.
--
-- Until now the platform learned the energy only from the stop receipt, so the app could show
-- nothing during a charge (0.00 kWh / 0.00 元) or a guess. Chargers report their running meter
-- while charging (OCPP MeterValues in the field); these columns hold the latest reading and the
-- device fact time it belongs to.
--
-- They are deliberately separate from energy_wh/amount_cents, which stay the settled figures the
-- bill, the revenue statistics and the bill detail are built from: a live reading must never leak
-- into settled money.
ALTER TABLE charging_orders
    ADD COLUMN IF NOT EXISTS metered_energy_wh BIGINT NOT NULL DEFAULT 0
        CHECK (metered_energy_wh >= 0),
    ADD COLUMN IF NOT EXISTS metered_at TIMESTAMPTZ;
