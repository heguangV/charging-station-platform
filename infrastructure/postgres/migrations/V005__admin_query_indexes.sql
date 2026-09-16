CREATE INDEX ix_flow_created ON charging_flow(created_at, flow_no);
CREATE INDEX ix_flow_station_active ON charging_flow(station_id)
  WHERE status IN (10, 20, 30, 40, 50, 80);
CREATE INDEX ix_flow_charger_active ON charging_flow(charger_id)
  WHERE charger_id IS NOT NULL AND status IN (10, 20, 30, 40, 50, 80);
