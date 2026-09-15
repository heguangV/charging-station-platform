UPDATE ml_task SET status = 'FAILED',
  finished_at = EXTRACT(EPOCH FROM clock_timestamp())::BIGINT,
  error_summary = '迁移时清理重复运行任务'
WHERE status IN ('PENDING', 'RUNNING') AND EXISTS(
  SELECT 1 FROM ml_task newer WHERE newer.task_type = ml_task.task_type
    AND newer.status IN ('PENDING', 'RUNNING')
    AND (newer.created_at > ml_task.created_at OR
      (newer.created_at = ml_task.created_at AND newer.task_no > ml_task.task_no))
);
CREATE UNIQUE INDEX ux_ml_task_one_running_type ON ml_task(task_type)
  WHERE status IN ('PENDING', 'RUNNING');

CREATE TABLE station_hourly_metric(
  station_id BIGINT NOT NULL REFERENCES station(id),
  bucket_at BIGINT NOT NULL,
  energy_mwh BIGINT NOT NULL CHECK(energy_mwh >= 0),
  order_count BIGINT NOT NULL CHECK(order_count >= 0),
  fast_order_count BIGINT NOT NULL CHECK(fast_order_count >= 0),
  slow_order_count BIGINT NOT NULL CHECK(slow_order_count >= 0),
  busy_device_seconds BIGINT NOT NULL CHECK(busy_device_seconds >= 0),
  refreshed_at BIGINT NOT NULL,
  PRIMARY KEY(station_id, bucket_at)
);
CREATE INDEX ix_station_hourly_bucket ON station_hourly_metric(bucket_at, station_id);

CREATE TABLE model_version(
  version_no TEXT PRIMARY KEY,
  task_no TEXT NOT NULL UNIQUE REFERENCES ml_task(task_no),
  algorithm TEXT NOT NULL,
  feature_schema_version INTEGER NOT NULL,
  random_seed BIGINT NOT NULL,
  train_from_at BIGINT NOT NULL,
  train_to_at BIGINT NOT NULL,
  mae DOUBLE PRECISION NOT NULL CHECK(mae >= 0),
  rmse DOUBLE PRECISION NOT NULL CHECK(rmse >= 0),
  mape DOUBLE PRECISION NOT NULL CHECK(mape >= 0),
  wape DOUBLE PRECISION NOT NULL CHECK(wape >= 0),
  baseline_mae DOUBLE PRECISION NOT NULL CHECK(baseline_mae >= 0),
  baseline_rmse DOUBLE PRECISION NOT NULL CHECK(baseline_rmse >= 0),
  excluded_sample_count BIGINT NOT NULL CHECK(excluded_sample_count >= 0),
  qualified SMALLINT NOT NULL CHECK(qualified IN (0, 1)),
  artifact_checksum TEXT NOT NULL,
  artifact_path TEXT NOT NULL,
  created_at BIGINT NOT NULL
);
CREATE INDEX ix_model_version_retention ON model_version(created_at);

CREATE TABLE load_prediction(
  station_id BIGINT NOT NULL REFERENCES station(id),
  model_version_no TEXT NOT NULL,
  generated_at BIGINT NOT NULL,
  target_at BIGINT NOT NULL,
  horizon_hour INTEGER NOT NULL CHECK(horizon_hour IN (1, 6, 24)),
  predicted_energy_mwh BIGINT NOT NULL CHECK(predicted_energy_mwh >= 0),
  predicted_free_count INTEGER NOT NULL CHECK(predicted_free_count >= 0),
  is_peak SMALLINT NOT NULL CHECK(is_peak IN (0, 1)),
  stale SMALLINT NOT NULL DEFAULT 0 CHECK(stale IN (0, 1)),
  PRIMARY KEY(station_id, model_version_no, target_at)
);
CREATE INDEX ix_load_prediction_query ON load_prediction(target_at, horizon_hour, station_id);

CREATE TABLE dashboard_state(
  singleton SMALLINT PRIMARY KEY CHECK(singleton = 1),
  data_version BIGINT NOT NULL CHECK(data_version >= 0)
);
INSERT INTO dashboard_state(singleton, data_version) VALUES(1, 0);
