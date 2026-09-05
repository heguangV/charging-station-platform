#pragma once

#include <cstdint>

struct sqlite3;

namespace ncs::infrastructure::sqlite
{

// UC-D-02 full demo seed, executed as migration v8. Inserts the five fixed
// Beijing stations, 48 chargers, five-zone tariffs and a deterministic 90-day
// history (300 owners, ~9000 orders, ~900 recharges) anchored at `anchorAt`.
//
// Must be called inside the transaction already opened by the caller; the v8
// schema_version row is committed as part of the seed. Pre-existing accounts
// that collide with the fixed seed identity set (sim_owner_001..300 or phones
// 13800001001..300) abort the seed with an explicit conflict error — the
// caller rolls the transaction back and the database keeps its v1-v7 data.
// The marker and the seed rows are committed in one transaction, so a
// marker-less database that still carries seed rows is externally tampered
// state; it aborts with the same conflict error instead of being silently
// repaired (recover from a backup). Stations, chargers and tariffs outside
// that conflict check are INSERT OR IGNORE / guarded UPDATEs, so re-runs
// reuse existing rows instead of duplicating them.
//
// Throws std::runtime_error on failure; the caller rolls the transaction back.
void applyFullDemoSeed(sqlite3* database, std::int64_t anchorAt);

// Legacy-station removal policy, also re-applied on opens where v8 has already
// run: the v1 migration body executes unconditionally on every open, so device
// rows removed by a previous seed reappear at freed ids unless purged. Rows of
// a station that no longer exists (by code prefix) are deleted; retained,
// referenced XEQ/CBD leftovers are kept. Throws std::runtime_error on failure.
void removeLegacyStations(sqlite3* database);

} // namespace ncs::infrastructure::sqlite
