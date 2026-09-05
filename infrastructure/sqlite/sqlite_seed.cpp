#include "infrastructure/sqlite/sqlite_seed.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ncs::infrastructure::sqlite
{
namespace
{

using std::int64_t;

constexpr int kOwnerCount = 300;
constexpr int kHistoryDays = 90;
constexpr int kDaySeconds = 86400, kHourSeconds = 3600;
constexpr int kTimeScale = 60;
// Newest created-second of a session keeps settle/cancel tails inside its UTC day.
constexpr int kDayEndCap = 85499;
// Seed numbers start at +8000 per prefix/date, clear of live business_sequence.
constexpr int kSequenceOffset = 8000;
constexpr std::uint64_t kSeedStream = 20260901ULL;

struct Site
{
    const char* code;
    const char* name;
    const char* address;
    const char* adcode;
    int64_t latitudeE6;
    int64_t longitudeE6;
    int dc120;
    int dc60;
    int ac;
    int share; // percent of order volume, in kSites order
    int electricityCent;
    int serviceCent;
};

// SRS UC-D-02 fixed network; ZGC already exists as the v1 legacy station and
// receives its top-up devices here, the four other sites are added by v8.
constexpr Site kSites[] = {
    {"ZGC", "NCS 中关村充电站", "北京市海淀区中关村大街 27 号", "110108", 39977680, 116316417, 4, 2,
     4, 24, 85, 50},
    {"CYGY", "NCS 朝阳公园充电站", "北京市朝阳区朝阳公园南路 1 号", "110105", 39933660, 116480863,
     6, 2, 4, 28, 90, 55},
    {"BJN", "NCS 北京南站充电站", "北京市丰台区北京南站南广场", "110106", 39858897, 116410717, 4, 2,
     4, 22, 80, 45},
    {"SJS", "NCS 石景山充电站", "北京市石景山区石景山路 68 号", "110107", 39923461, 116150611, 2, 2,
     4, 14, 75, 40},
    {"TZYH", "NCS 通州运河充电站", "北京市通州区通胡大街 70 号", "110112", 39910655, 116679698, 2,
     2, 4, 12, 70, 35},
};

constexpr int kSiteCount = static_cast<int>(std::size(kSites));
// Cumulative order-volume thresholds of kSites (24/52/74/88/100 percent).
constexpr int kSiteThreshold[] = {24, 52, 74, 88, 100};

struct ChargerRef
{
    int64_t id;
    int powerWatt;
    std::string code;
};

struct Session
{
    int owner, site, hour, sec, delay, duration;
    bool fast, cancelled;
    int64_t chargerId, powerWatt, created, confirmAt, cancelAt, startAt, settleAt;
    int64_t energyMwh = 0, amountCent = 0, paidCent = 0, balanceAfterCent = 0;
    std::string chargerCode, flowNo, orderNo;
};

struct WalletEvent
{
    int type; // 0 recharge, 1 charge
    int64_t at;
    int64_t amountCent; // negative for a charge
    int64_t balanceAfterCent;
    std::string transactionNo, relatedNo;
};

class Stmt final
{
  public:
    Stmt(sqlite3* database, const char* sql) : database_(database)
    {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK)
            fail();
    }

    ~Stmt()
    {
        sqlite3_finalize(statement_);
    }

    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    void bind(const int index, const int64_t value)
    {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK)
            fail();
    }

    void bind(const int index, const std::string_view value)
    {
        if (sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK)
            fail();
    }

    void bindNull(const int index)
    {
        if (sqlite3_bind_null(statement_, index) != SQLITE_OK)
            fail();
    }

    bool step()
    {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW)
            return true;
        if (result == SQLITE_DONE)
            return false;
        fail();
    }

    void execute()
    {
        (void)step();
    }

    int64_t integer(const int column) const
    {
        return sqlite3_column_int64(statement_, column);
    }

    std::string text(const int column) const
    {
        const auto* cell = sqlite3_column_text(statement_, column);
        return cell ? std::string(reinterpret_cast<const char*>(cell)) : std::string();
    }

    void reset()
    {
        sqlite3_reset(statement_);
        sqlite3_clear_bindings(statement_);
    }

  private:
    [[noreturn]] void fail() const
    {
        throw std::runtime_error(sqlite3_errmsg(database_));
    }

    sqlite3* database_;
    sqlite3_stmt* statement_;
};

void bindValue(Stmt& statement, const int index, const int64_t value)
{
    statement.bind(index, value);
}

void bindValue(Stmt& statement, const int index, const std::string_view value)
{
    statement.bind(index, value);
}

void bindFrom(Stmt& statement, int index)
{
    (void)statement;
    (void)index;
}

template <typename First, typename... Rest>
void bindFrom(Stmt& statement, int index, First first, Rest... rest)
{
    bindValue(statement, index++, first);
    bindFrom(statement, index, rest...);
}

template <typename... Values> void bindAll(Stmt& statement, Values... values)
{
    bindFrom(statement, 1, values...);
}

void bindNullOr(Stmt& statement, const int index, const bool isNull, const int64_t value)
{
    if (isNull)
        statement.bindNull(index);
    else
        statement.bind(index, value);
}

int64_t maxId(sqlite3* database, const char* table)
{
    const std::string sql = std::string("SELECT COALESCE(MAX(id),0) FROM ") + table;
    Stmt query(database, sql.c_str());
    if (!query.step())
        throw std::runtime_error("MAX(id) query failed");
    return query.integer(0);
}

// YYYYMMDD of the UTC day containing `seconds` (civil-from-days, portable).
std::string dateString(int64_t seconds)
{
    int64_t z = seconds / kDaySeconds + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t year = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp < 10 ? mp + 3 : mp - 9;
    year += month <= 2 ? 1 : 0;
    char buffer[9];
    std::snprintf(buffer, sizeof(buffer), "%04lld%02u%02u", static_cast<long long>(year), month,
                  day);
    return std::string(buffer, 8);
}

std::string businessNo(const char* prefix, const std::string& date, const int sequence)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%s%s%04d", prefix, date.c_str(), sequence);
    return buffer;
}

} // namespace

// Removes the v1 demo stations (XEQ/CBD) only when neither they nor their
// devices are referenced by any business row; referenced leftovers are kept
// and the deviation is documented in database-design.md.
void removeLegacyStations(sqlite3* database)
{
    // The v1 migration body re-executes on every open, so removed devices can
    // reappear at freed ids; purge rows whose legacy station no longer exists.
    Stmt purgeArtifacts(database, "DELETE FROM charger WHERE (code LIKE 'XEQ-%' OR code LIKE "
                                  "'CBD-%') AND NOT EXISTS (SELECT 1 FROM station WHERE "
                                  "code = SUBSTR(charger.code, 1, 3))");
    purgeArtifacts.execute();
    Stmt find(database, "SELECT id FROM station WHERE code=?1");
    Stmt references(database, "SELECT (SELECT COUNT(*) FROM charging_flow WHERE station_id=?1)"
                              "+(SELECT COUNT(*) FROM charging_order WHERE station_id=?1)"
                              "+(SELECT COUNT(*) FROM flow_queue WHERE station_id=?1)"
                              "+(SELECT COUNT(*) FROM price_adjustment WHERE station_id=?1)"
                              "+(SELECT COUNT(*) FROM station_hourly_metric WHERE station_id=?1)"
                              "+(SELECT COUNT(*) FROM load_prediction WHERE station_id=?1)"
                              "+(SELECT COUNT(*) FROM device_command WHERE charger_id IN"
                              "(SELECT id FROM charger WHERE station_id=?1))");
    Stmt deleteChargers(database, "DELETE FROM charger WHERE station_id=?1");
    Stmt deleteStation(database, "DELETE FROM station WHERE id=?1");
    for (const char* code : {"XEQ", "CBD"})
    {
        find.reset();
        find.bind(1, code);
        if (!find.step())
            continue;
        const int64_t stationId = find.integer(0);
        references.reset();
        references.bind(1, stationId);
        if (!references.step() || references.integer(0) != 0)
            continue;
        deleteChargers.reset();
        deleteChargers.bind(1, stationId);
        deleteChargers.execute();
        deleteStation.reset();
        deleteStation.bind(1, stationId);
        deleteStation.execute();
    }
}

// Upserts the fixed network (five stations, 48 devices, five tariffs) and
// loads the idle non-faulty devices generated orders may use, split by type
// into pools[site][fast=0][slow=1].
void reconcileFleet(sqlite3* database, std::vector<ChargerRef> pools[kSiteCount][2],
                    int64_t siteStationIds[kSiteCount])
{
    removeLegacyStations(database);
    int64_t stationId = maxId(database, "station") + 1;
    Stmt findStation(database, "SELECT id FROM station WHERE code=?1");
    Stmt insertStation(
        database,
        "INSERT OR IGNORE INTO station(id,code,name,address,adcode,latitude_e6,longitude_e6,"
        "business_hours,enabled) VALUES(?1,?2,?3,?4,?5,?6,?7,'00:00-24:00',1)");
    for (int siteIndex = 0; siteIndex < kSiteCount; ++siteIndex)
    {
        const auto& site = kSites[siteIndex];
        findStation.reset();
        findStation.bind(1, site.code);
        if (findStation.step())
        {
            siteStationIds[siteIndex] = findStation.integer(0);
            continue;
        }
        insertStation.reset();
        bindAll(insertStation, stationId, site.code, site.name, site.address, site.adcode,
                site.latitudeE6, site.longitudeE6);
        insertStation.execute();
        siteStationIds[siteIndex] = stationId++;
    }

    Stmt insertCharger(database,
                       "INSERT OR IGNORE INTO charger(id,station_id,code,charger_type,power_watt,"
                       "connector_standard,status) VALUES(?1,?2,?3,?4,?5,?6,0)");
    int64_t chargerId = maxId(database, "charger") + 1;
    for (int siteIndex = 0; siteIndex < kSiteCount; ++siteIndex)
    {
        const auto& site = kSites[siteIndex];
        char code[24];
        for (const bool dc : {true, false})
        {
            const int total = dc ? site.dc60 + site.dc120 : site.ac;
            for (int index = 1; index <= total; ++index)
            {
                std::snprintf(code, sizeof(code), "%s-%s-%02d", site.code, dc ? "DC" : "AC", index);
                insertCharger.reset();
                bindAll(insertCharger, chargerId++, siteStationIds[siteIndex], code, dc ? 1 : 0,
                        dc ? index <= site.dc60 ? 60000 : 120000 : 7000,
                        dc ? "GB/T 20234.3" : "GB/T 20234.2");
                insertCharger.execute();
            }
        }
    }

    // The six fixed codes below are the faulty devices of every seeded database.
    Stmt fault(database, "UPDATE charger SET status=2 WHERE status<>2 AND code IN ('ZGC-DC-01',"
                         "'CYGY-DC-01','CYGY-AC-01','BJN-DC-01','SJS-AC-01','TZYH-DC-01')");
    fault.execute();

    // v1 rows carry the bare "GB/T" connector; normalize them per type.
    Stmt normalize(database,
                   "UPDATE charger SET connector_standard=CASE charger_type WHEN 1 THEN "
                   "'GB/T 20234.3' ELSE 'GB/T 20234.2' END WHERE connector_standard='GB/T'");
    normalize.execute();

    // 110105 is repaired from the v1 signature 92/48 to the SRS value 90/55;
    // the remaining three zones are added when absent.
    Stmt tariffRepair(database, "UPDATE region_tariff SET electricity_cent_per_kwh=90,"
                                "service_cent_per_kwh=55 WHERE adcode='110105' AND "
                                "electricity_cent_per_kwh=92 AND service_cent_per_kwh=48 AND "
                                "effective_from=0");
    tariffRepair.execute();
    Stmt tariffInsert(database,
                      "INSERT OR IGNORE INTO region_tariff(adcode,electricity_cent_per_kwh,"
                      "service_cent_per_kwh,effective_from,effective_to) VALUES"
                      "('110106',80,45,0,4102444800),('110107',75,40,0,4102444800),"
                      "('110112',70,35,0,4102444800)");
    tariffInsert.execute();

    Stmt pool(database, "SELECT id,power_watt,charger_type,code FROM charger WHERE station_id=?1 "
                        "AND status<>2 ORDER BY id");
    for (int siteIndex = 0; siteIndex < kSiteCount; ++siteIndex)
    {
        pools[siteIndex][0].clear();
        pools[siteIndex][1].clear();
        pool.reset();
        pool.bind(1, siteStationIds[siteIndex]);
        while (pool.step())
        {
            const int type = static_cast<int>(pool.integer(2));
            pools[siteIndex][type == 1 ? 0 : 1].push_back(
                ChargerRef{pool.integer(0), static_cast<int>(pool.integer(1)), pool.text(3)});
        }
    }
}

// Fails loudly when a pre-existing account collides with the fixed seed
// identity set (usernames sim_owner_001..300, phones 13800001001..300): the
// seed inserts users with plain INSERT, so a collision would otherwise abort
// mid-seed with an opaque constraint message and no recovery hint. Matching is
// exact — look-alike usernames and other phone prefixes are left untouched.
void validateSeedConflicts(sqlite3* database)
{
    Stmt find(database, "SELECT username,phone FROM user_account WHERE username=?1 OR phone=?2 "
                        "LIMIT 1");
    for (int owner = 0; owner < kOwnerCount; ++owner)
    {
        char username[24];
        char phone[16];
        std::snprintf(username, sizeof(username), "sim_owner_%03d", owner + 1);
        std::snprintf(phone, sizeof(phone), "1380000%04d", 1001 + owner);
        find.reset();
        find.bind(1, username);
        find.bind(2, phone);
        if (find.step())
        {
            if (find.text(0) == username)
                throw std::runtime_error("UC-D-02 seed conflict: user_account '" +
                                         std::string(username) +
                                         "' already exists; resolve the conflicting account "
                                         "before upgrading to schema v8");
            throw std::runtime_error("UC-D-02 seed conflict: phone '" + std::string(phone) +
                                     "' of user '" + find.text(0) +
                                     "' collides with the seed phone set; resolve the "
                                     "conflicting account before upgrading to schema v8");
        }
    }
}

void applyFullDemoSeed(sqlite3* database, const int64_t anchorAt)
{
    validateSeedConflicts(database);
    const int64_t anchorDayStart = anchorAt / kDaySeconds * kDaySeconds;
    const int64_t day0Start = anchorDayStart - int64_t(kHistoryDays) * kDaySeconds;
    const int64_t day0EpochDay = day0Start / kDaySeconds;
    std::vector<ChargerRef> pools[kSiteCount][2];
    int64_t siteStationIds[kSiteCount] = {};
    reconcileFleet(database, pools, siteStationIds);
    // Generation: 80-120 sessions per day over the 90 days before the anchor.
    std::mt19937_64 rng(kSeedStream);
    std::vector<int> drawPool(kOwnerCount);
    std::iota(drawPool.begin(), drawPool.end(), 0);
    std::array<int, kHistoryDays> dayCounters = {};
    std::vector<Session> sessions;
    std::vector<std::vector<int>> ownerSessions(kOwnerCount);
    sessions.reserve(10000);
    for (int day = 0; day < kHistoryDays; ++day)
    {
        const int64_t dayStart = day0Start + int64_t(day) * kDaySeconds;
        const std::string date = dateString(dayStart);
        const int count = 80 + static_cast<int>(rng() % 41);
        for (int i = 0; i < count; ++i)
        {
            const int j = i + static_cast<int>(rng() % (kOwnerCount - i));
            std::swap(drawPool[i], drawPool[j]);
        }
        const bool weekday = ((day0EpochDay + day + 3) % 7) < 5;
        for (int slot = 0; slot < count; ++slot)
        {
            Session session;
            session.owner = drawPool[slot];
            session.cancelled = rng() % 100 < 6;
            session.fast = rng() % 100 < 70;
            const int shareDraw = static_cast<int>(rng() % 100);
            int site = 0;
            while (shareDraw >= kSiteThreshold[site])
                ++site;
            session.site = site;
            int weights[24];
            for (int hour = 0; hour < 24; ++hour)
            {
                const bool peak = weekday ? (hour >= 7 && hour <= 8) || (hour >= 17 && hour <= 20)
                                          : hour >= 10 && hour <= 19;
                weights[hour] = peak ? 5 : 1;
            }
            int draw = static_cast<int>(rng() % (weekday ? 48 : 64));
            int hour = 0;
            while (draw >= weights[hour])
                draw -= weights[hour++];
            session.hour = hour;
            const int upper = std::min(3300, kDayEndCap - hour * kHourSeconds);
            session.sec = static_cast<int>(rng() % (upper + 1));
            session.delay = 30 + static_cast<int>(rng() % 211);
            session.duration = session.fast ? 20 + static_cast<int>(rng() % 46)
                                            : 120 + static_cast<int>(rng() % 481);
            const auto& chargerPool = pools[session.site][session.fast ? 0 : 1];
            const ChargerRef& charger = chargerPool[rng() % chargerPool.size()];
            session.chargerId = charger.id;
            session.powerWatt = charger.powerWatt;
            session.chargerCode = charger.code;
            session.created = dayStart + int64_t(hour) * kHourSeconds + session.sec;
            session.confirmAt =
                session.created + 10 + static_cast<int>(rng() % (session.delay - 19));
            session.cancelAt =
                session.cancelled ? session.created + 240 + static_cast<int>(rng() % 661) : 0;
            session.startAt = session.created + session.delay;
            session.settleAt = session.startAt + session.duration;
            if (!session.cancelled)
            {
                session.energyMwh =
                    int64_t(session.powerWatt) * session.duration * kTimeScale * 10 / 36;
                session.amountCent = (session.energyMwh * (kSites[session.site].electricityCent +
                                                           kSites[session.site].serviceCent) +
                                      500000) /
                                     1000000;
            }
            const int sequence = kSequenceOffset + ++dayCounters[day];
            session.flowNo = businessNo("FL", date, sequence);
            session.orderNo = businessNo("OR", date, sequence);
            ownerSessions[drawPool[slot]].push_back(static_cast<int>(sessions.size()));
            sessions.push_back(std::move(session));
        }
    }
    // Wallet replay, chronological per owner: a recharge (6x-14x the upcoming
    // bill) fills the gap before every session the balance cannot cover.
    std::vector<std::vector<WalletEvent>> ownerEvents(kOwnerCount);
    std::vector<int64_t> registeredAt(kOwnerCount, 0);
    std::vector<int64_t> finalBalance(kOwnerCount, 0);
    std::map<std::string, int> rechargeNumbers;
    std::map<std::string, int> walletNumbers;
    for (int owner = 0; owner < kOwnerCount; ++owner)
    {
        int64_t balance = 0;
        int64_t registered = 0;
        int64_t lastSettle = 0;
        for (const int sessionIndex : ownerSessions[owner])
        {
            Session& session = sessions[sessionIndex];
            if (registered == 0)
                registered = session.created - (kDaySeconds + rng() % (29 * kDaySeconds));
            if (session.cancelled)
                continue;
            if (balance < session.amountCent)
            {
                const int64_t rechargeCent =
                    (session.amountCent * (6000 + rng() % 8001) / 1000 + 50) / 100 * 100;
                const int64_t gapStart = lastSettle == 0 ? registered : lastSettle;
                const int64_t gap = session.created - gapStart;
                const int64_t at =
                    std::min(gapStart + 1 + (gap > 2 ? static_cast<int64_t>(rng() % (gap - 2)) : 0),
                             session.created - 1);
                balance += rechargeCent;
                const std::string date = dateString(at);
                const int sequence = kSequenceOffset + ++rechargeNumbers[date];
                const std::string rechargeNo = businessNo("RC", date, sequence);
                const std::string txnNo =
                    businessNo("WT", date, kSequenceOffset + ++walletNumbers[date]);
                ownerEvents[owner].push_back(
                    WalletEvent{0, at, rechargeCent, balance, txnNo, rechargeNo});
            }
            balance -= session.amountCent;
            session.paidCent = session.amountCent;
            session.balanceAfterCent = balance;
            const std::string date = dateString(session.settleAt);
            const std::string txnNo =
                businessNo("WT", date, kSequenceOffset + ++walletNumbers[date]);
            ownerEvents[owner].push_back(WalletEvent{1, session.settleAt, -session.amountCent,
                                                     balance, txnNo, session.orderNo});
            lastSettle = session.settleAt;
        }
        registeredAt[owner] = registered;
        finalBalance[owner] = balance;
    }

    std::vector<int64_t> ownerIds(kOwnerCount, 0);
    Stmt insertUser(
        database,
        "INSERT INTO user_account(username,phone,nickname,status,registered_at,balance_cent,"
        "debt_cent,has_active_flow,version,deleted) VALUES(?1,?2,?3,1,?4,?5,0,0,1,0)");
    for (int owner = 0; owner < kOwnerCount; ++owner)
    {
        char username[24];
        char phone[16];
        char nickname[24];
        std::snprintf(username, sizeof(username), "sim_owner_%03d", owner + 1);
        std::snprintf(phone, sizeof(phone), "1380000%04d", 1001 + owner);
        std::snprintf(nickname, sizeof(nickname), "模拟车主 %03d", owner + 1);
        insertUser.reset();
        bindAll(insertUser, username, phone, nickname, registeredAt[owner], finalBalance[owner]);
        insertUser.execute();
        ownerIds[owner] = sqlite3_last_insert_rowid(database);
    }
    Stmt insertFlow(
        database,
        "INSERT INTO charging_flow(flow_no,user_id,station_id,charger_type,charger_id,"
        "charger_code,status,started_at,version,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,"
        "?10)");
    Stmt insertOrder(
        database,
        "INSERT INTO charging_order(order_no,flow_no,user_id,station_id,station_name,"
        "charger_id,charger_code,charger_type,electricity_price,service_price,power_watt,"
        "time_scale,target_amount_cent,status,created_at,started_at,ended_at,energy_mwh,"
        "amount_cent,paid_cent,debt_added_cent,balance_after_cent,debt_after_cent,settled_at)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,NULL,?13,?14,?15,?16,?17,?18,?19,0,"
        "?20,0,?21)");
    Stmt insertEvent(database,
                     "INSERT INTO flow_event(flow_no,from_status,to_status,reason_code,at) "
                     "VALUES(?1,?2,?3,?4,?5)");
    std::map<int64_t, std::pair<int64_t, int64_t>> chargerTotals;
    for (const Session& session : sessions)
    {
        const int64_t ownerId = ownerIds[session.owner];
        const auto& site = kSites[session.site];
        const auto& code = session.chargerCode;
        const bool cancelled = session.cancelled;

        insertFlow.reset();
        bindAll(insertFlow, session.flowNo, ownerId, siteStationIds[session.site],
                session.fast ? 1 : 0, session.chargerId, code, cancelled ? 70 : 60);
        bindNullOr(insertFlow, 8, cancelled, session.startAt);
        bindFrom(insertFlow, 9, cancelled ? 3 : 4, session.created);
        insertFlow.execute();

        insertOrder.reset();
        bindFrom(insertOrder, 1, session.orderNo, session.flowNo, ownerId,
                 siteStationIds[session.site], site.name, session.chargerId, code,
                 session.fast ? 1 : 0, site.electricityCent, site.serviceCent, session.powerWatt,
                 kTimeScale);
        bindFrom(insertOrder, 13, cancelled ? 70 : 60, session.confirmAt);
        bindNullOr(insertOrder, 15, cancelled, session.startAt);
        bindNullOr(insertOrder, 16, cancelled, session.settleAt);
        bindFrom(insertOrder, 17, session.energyMwh, session.amountCent, session.paidCent,
                 session.balanceAfterCent);
        bindNullOr(insertOrder, 21, cancelled, session.settleAt);
        insertOrder.execute();

        // Events: 10->20, 20->30, then 30->40+40->60 or USER_CANCELLED 30->70.
        struct Transition
        {
            int from;
            int to;
            const char* reason;
            int64_t at;
        };
        Transition transitions[4];
        int transitionCount = 0;
        transitions[transitionCount++] = {10, 20, "FLOW_CREATED", session.created};
        transitions[transitionCount++] = {20, 30, "QUOTE_CONFIRMED", session.confirmAt};
        if (cancelled)
        {
            transitions[transitionCount++] = {30, 70, "USER_CANCELLED", session.cancelAt};
        }
        else
        {
            transitions[transitionCount++] = {30, 40, "CHARGING_STARTED", session.startAt};
            transitions[transitionCount++] = {40, 60, "USER_STOPPED", session.settleAt};
            auto& totals = chargerTotals[session.chargerId];
            ++totals.first;
            totals.second += session.settleAt - session.startAt;
        }
        for (int index = 0; index < transitionCount; ++index)
        {
            insertEvent.reset();
            bindAll(insertEvent, session.flowNo, transitions[index].from, transitions[index].to,
                    transitions[index].reason, transitions[index].at);
            insertEvent.execute();
        }
    }

    Stmt insertWallet(database, "INSERT INTO wallet_account(user_id,balance_cent,debt_cent,version,"
                                "updated_at) VALUES(?1,?2,0,?3,?4)");
    Stmt insertTxn(database,
                   "INSERT INTO wallet_transaction(user_id,transaction_no,type,amount_cent,"
                   "balance_after_cent,debt_after_cent,related_no,created_at) "
                   "VALUES(?1,?2,?3,?4,?5,0,?6,?7)");
    Stmt insertRecharge(database,
                        "INSERT INTO recharge_order(recharge_no,user_id,requested_cent,"
                        "debt_paid_cent,balance_added_cent,balance_after_cent,debt_after_cent,"
                        "completed_at) VALUES(?1,?2,?3,0,?3,?4,0,?5)");
    Stmt updateUserBalance(database, "UPDATE user_account SET balance_cent=?1 WHERE id=?2");
    for (int owner = 0; owner < kOwnerCount; ++owner)
    {
        const std::vector<WalletEvent>& events = ownerEvents[owner];
        insertWallet.reset();
        bindAll(insertWallet, ownerIds[owner], finalBalance[owner],
                1 + static_cast<int64_t>(events.size()),
                events.empty() ? registeredAt[owner] : events.back().at);
        insertWallet.execute();
        for (const WalletEvent& event : events)
        {
            insertTxn.reset();
            bindAll(insertTxn, ownerIds[owner], event.transactionNo, event.type, event.amountCent,
                    event.balanceAfterCent, event.relatedNo, event.at);
            insertTxn.execute();
            if (event.type == 0)
            {
                insertRecharge.reset();
                bindAll(insertRecharge, event.relatedNo, ownerIds[owner], event.amountCent,
                        event.balanceAfterCent, event.at);
                insertRecharge.execute();
            }
        }
        updateUserBalance.reset();
        bindAll(updateUserBalance, finalBalance[owner], ownerIds[owner]);
        updateUserBalance.execute();
    }

    Stmt updateCharger(database, "UPDATE charger SET total_count=?1,total_minutes=?2 WHERE "
                                 "id=?3");
    for (const auto& [chargerId, totals] : chargerTotals)
    {
        updateCharger.reset();
        bindAll(updateCharger, totals.first, totals.second, chargerId);
        updateCharger.execute();
    }

    Stmt marker(database, "INSERT INTO schema_version(version,name,checksum,applied_at) VALUES(8,"
                          "'full-demo-seed','ncs-v8-full-demo-seed',?1)");
    marker.bind(1, anchorAt);
    marker.execute();
}

} // namespace ncs::infrastructure::sqlite