package postgres

import (
	"context"
	"io/fs"
	"os"
	"strconv"
	"testing"
	"testing/fstest"
	"time"

	"github.com/heguangV/charging-station-platform/backend/migrations"
)

// Exercise the production runner, including existing rows, rather than replaying
// SQL manually. Each path owns a scratch database so schema-reset tests cannot interfere.
func TestMigrationFreshAndV9Upgrade(t *testing.T) {
	base := os.Getenv("NCS_TEST_PG_DSN")
	if base == "" {
		t.Skip("NCS_TEST_PG_DSN not set")
	}
	for _, upgrade := range []bool{false, true} {
		t.Run(map[bool]string{false: "empty", true: "v9_with_data"}[upgrade], func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()
			name := "ncs_upgrade_" + strconv.FormatInt(time.Now().UnixNano(), 10)
			maintenance := dsnForDatabase(t, base, "postgres")
			admin, err := Open(ctx, maintenance, 1)
			if err != nil {
				t.Fatal(err)
			}
			defer admin.Close()
			if _, err := admin.ExecContext(ctx, `CREATE DATABASE `+quoteIdentifier(name)); err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() {
				if err := dropDatabase(context.Background(), maintenance, name); err != nil {
					t.Error(err)
				}
			})
			db, err := Open(ctx, dsnForDatabase(t, base, name), 2)
			if err != nil {
				t.Fatal(err)
			}
			defer db.Close()
			runner, err := NewSQLDB(db)
			if err != nil {
				t.Fatal(err)
			}
			var orderNo string
			if upgrade {
				earlier := fstest.MapFS{}
				entries, err := fs.ReadDir(migrations.FS, ".")
				if err != nil {
					t.Fatal(err)
				}
				for _, entry := range entries {
					version, err := strconv.Atoi(entry.Name()[:4])
					if err != nil || version > 9 {
						continue
					}
					contents, err := migrations.FS.ReadFile(entry.Name())
					if err != nil {
						t.Fatal(err)
					}
					earlier[entry.Name()] = &fstest.MapFile{Data: contents}
				}
				report, err := Run(ctx, runner, earlier)
				if err != nil || len(report.Applied) != 9 {
					t.Fatalf("v9 migration: %v, %v", report, err)
				}
				user, station, _, charger := orderFixture(t, db, ctx, uniqueSuffix(t))
				orderNo = "UPGRADE-ORDER"
				if _, err := db.ExecContext(ctx, `INSERT INTO charging_orders(order_no,user_id,station_id,charger_id,status) VALUES($1,$2,$3,$4,'COMPLETED')`, orderNo, user, station, charger); err != nil {
					t.Fatal(err)
				}
				if _, err := db.ExecContext(ctx, `INSERT INTO order_appeals(order_no,user_id,reason) VALUES($1,$2,'upgrade check')`, orderNo, user); err != nil {
					t.Fatal(err)
				}
				if _, err := db.ExecContext(ctx, `INSERT INTO admin_accounts(username,password_hash,role) VALUES('upgrade-admin','test-hash','SUPER_ADMIN')`); err != nil {
					t.Fatal(err)
				}
			}
			report, err := Run(ctx, runner, migrations.FS)
			want := 12
			if upgrade {
				want = 3
			}
			if err != nil || len(report.Applied) != want {
				t.Fatalf("full migration: %v, %v", report, err)
			}
			repeated, err := Run(ctx, runner, migrations.FS)
			if err != nil || len(repeated.Applied) != 0 {
				t.Fatalf("repeat migration: %v, %v", repeated, err)
			}
			if upgrade {
				var valid bool
				if err := db.QueryRowContext(ctx, `SELECT metered_energy_wh=0 AND metered_at IS NULL FROM charging_orders WHERE order_no=$1`, orderNo).Scan(&valid); err != nil || !valid {
					t.Fatalf("order preserved: %v %v", valid, err)
				}
				if err := db.QueryRowContext(ctx, `SELECT status='PENDING' AND decision_reason IS NULL FROM order_appeals WHERE order_no=$1`, orderNo).Scan(&valid); err != nil || !valid {
					t.Fatalf("appeal preserved: %v %v", valid, err)
				}
				if err := db.QueryRowContext(ctx, `SELECT version=1 AND NOT must_change_password FROM admin_accounts WHERE username='upgrade-admin'`).Scan(&valid); err != nil || !valid {
					t.Fatalf("admin preserved: %v %v", valid, err)
				}
			}
		})
	}
}
