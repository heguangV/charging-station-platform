-- UC-D-03 订单确认与申诉：对齐 SQLite v10（ncs-v10-order-confirmation）。
-- 结算目标由"直接完成"改为状态 100 待用户确认；用户申诉进入 110 待审核。
-- 因此充电订单需要申诉与审核字段，活动流程与订单的状态约束需要容纳 100/110。
-- 版本号与校验和必须与 infrastructure/sqlite/sqlite_repository.cpp 的 v10 完全一致，
-- 否则 sqlite_to_postgres 的源库校验与两端契约测试会不一致。

ALTER TABLE charging_order ADD COLUMN appeal_reason TEXT NOT NULL DEFAULT '';
ALTER TABLE charging_order ADD COLUMN appeal_at BIGINT;
ALTER TABLE charging_order ADD COLUMN reviewed_by BIGINT;
ALTER TABLE charging_order ADD COLUMN reviewed_at BIGINT;

-- 原约束只允许 30~90。约束名由 PostgreSQL 隐式生成，这里按表名与定义模式查找后重建，
-- 不依赖自动命名的稳定性。注意 PostgreSQL 会把 `IN (…)` 规范化成 `= ANY (ARRAY[…])`，
-- 因此匹配的是规范化后的定义文本。
DO $$
DECLARE target text;
BEGIN
  SELECT con.conname INTO target
    FROM pg_constraint con JOIN pg_class rel ON rel.oid = con.conrelid
   WHERE rel.relname = 'charging_order' AND con.contype = 'c'
     AND pg_get_constraintdef(con.oid) = 'CHECK ((status = ANY (ARRAY[30, 40, 50, 60, 70, 80, 90])))';
  IF target IS NULL THEN
    RAISE EXCEPTION 'charging_order status check constraint not found';
  END IF;
  EXECUTE format('ALTER TABLE charging_order DROP CONSTRAINT %I', target);
END $$;
ALTER TABLE charging_order ADD CONSTRAINT charging_order_status_check
  CHECK(status IN (30, 40, 50, 60, 70, 80, 90, 100, 110));

-- 活动流程同样需要 100/110，并据此调整"每用户/每设备仅一个活动流程"的唯一索引谓词。
DO $$
DECLARE target text;
BEGIN
  SELECT con.conname INTO target
    FROM pg_constraint con JOIN pg_class rel ON rel.oid = con.conrelid
   WHERE rel.relname = 'charging_flow' AND con.contype = 'c'
     AND pg_get_constraintdef(con.oid) =
         'CHECK ((status = ANY (ARRAY[10, 20, 30, 40, 50, 60, 70, 80, 90])))';
  IF target IS NULL THEN
    RAISE EXCEPTION 'charging_flow status check constraint not found';
  END IF;
  EXECUTE format('ALTER TABLE charging_flow DROP CONSTRAINT %I', target);
END $$;
ALTER TABLE charging_flow ADD CONSTRAINT charging_flow_status_check
  CHECK(status IN (10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110));

DROP INDEX uq_active_flow_user;
CREATE UNIQUE INDEX uq_active_flow_user ON charging_flow(user_id)
  WHERE status IN (10, 20, 30, 40, 50, 80, 100, 110);

DROP INDEX uq_active_flow_charger;
CREATE UNIQUE INDEX uq_active_flow_charger ON charging_flow(charger_id)
  WHERE charger_id IS NOT NULL AND status IN (20, 30, 40, 50, 80, 100, 110);
