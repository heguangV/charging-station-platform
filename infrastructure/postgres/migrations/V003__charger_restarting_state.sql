ALTER TABLE charger DROP CONSTRAINT charger_status_check;
ALTER TABLE charger ADD CONSTRAINT charger_status_check CHECK(status IN (0, 1, 2, 3, 4));
