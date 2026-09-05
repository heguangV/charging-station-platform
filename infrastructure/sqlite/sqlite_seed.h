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
// schema_version row is committed as part of the seed. A run always unwinds
// the scope of any earlier marker-less run first (INSERT OR IGNORE / guarded
// UPDATEs keep the rest idempotent), so a database that lost its v8 marker
// re-seeds cleanly without duplicating rows and without touching v1-v7
// business data written outside the seed scope.
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
