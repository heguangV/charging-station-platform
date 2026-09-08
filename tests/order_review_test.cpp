// UC-U-12：真实 SQLite + Crow 契约、事务、并发与重启持久性。
#include "core/application/order_review_service.h"
#include "infrastructure/sqlite/sqlite_repository.h"
#include "server/controller/order_review_routes.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace ncs;
using namespace core::application;
namespace
{
int failures = 0;
void check(bool ok, const char* message)
{
    if (!ok)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
crow::response call(server::ServerApp& app, crow::HTTPMethod method, const std::string& order,
                    const std::string& token, const std::string& body = {},
                    const std::string& key = {})
{
    const auto url = "/api/v1/user/orders/" + order + "/review";
    crow::request req(method, url, url, crow::query_string(url), {}, body, 1, 1, true, false,
                      false);
    req.remote_ip_address = "127.0.0.1";
    req.add_header("Content-Type", "application/json");
    if (!token.empty())
        req.add_header("Authorization", "Bearer " + token);
    if (!key.empty())
        req.add_header("Idempotency-Key", key);
    crow::response response;
    app.handle_full(req, response);
    return response;
}
crow::response callStationWall(server::ServerApp& app, const std::int64_t station,
                               const std::string& token)
{
    const auto url = "/api/v1/user/stations/" + std::to_string(station) + "/reviews";
    crow::request req(crow::HTTPMethod::GET, url, url, crow::query_string(url), {}, {}, 1, 1, true,
                      false, false);
    req.remote_ip_address = "127.0.0.1";
    if (!token.empty())
        req.add_header("Authorization", "Bearer " + token);
    crow::response response;
    app.handle_full(req, response);
    return response;
}
QJsonObject data(const crow::response& response)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(response.body))
        .object()["data"]
        .toObject();
}
std::string body(int rating, const QString& content)
{
    return QJsonDocument(QJsonObject{{"rating", rating}, {"content", content}})
        .toJson(QJsonDocument::Compact)
        .toStdString();
}
std::string maskedPhone(const std::string& phone)
{
    return phone.size() == 11 ? phone.substr(0, 3) + "****" + phone.substr(7) : "***";
}
std::string expectedAuthor(const UserAccount& account)
{
    return account.nickname.empty() ? maskedPhone(account.phone) : account.nickname;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir temp;
    check(temp.isValid(), "isolated temporary directory");
    if (!temp.isValid())
        return 1;
    const auto path = temp.filePath("review.db").toStdString();
    std::string persistedOrder;
    std::int64_t persistedUser = 0;
    std::int64_t persistedWallStation = 0;
    const auto now = std::chrono::system_clock::now();
    {
        infrastructure::sqlite::SqliteRepository repository(path);
        std::vector<ChargingOrder> completed;
        for (const auto& order : repository.allOrders())
            if (order.status == 60 && order.settledAt)
                completed.push_back(order);
        check(completed.size() >= 5, "completed seeded orders available");
        if (completed.size() < 5)
            return 1;
        const auto order = completed[0];
        persistedOrder = order.orderNo;
        persistedUser = order.userId;
        OrderReviewService reviews(repository, repository);
        SessionManager sessions;
        const auto issued =
            sessions.issue("user:" + std::to_string(order.userId), "review-test", TokenKind::User,
                           {Role::User}, now, std::chrono::hours(1));
        const auto other = sessions.issue("user:999999", "review-other", TokenKind::User,
                                          {Role::User}, now, std::chrono::hours(1));
        const auto token = issued->accessToken;
        BoundedExecutor executor(2, 16);
        IdempotencyService idempotency(&repository);
        server::ServerApp app;
        server::controller::ApiRoutes api(app);
        server::controller::OrderReviewRoutes routes(api, reviews, sessions, executor, idempotency);
        app.validate();
        const auto get = crow::HTTPMethod::GET;
        const auto post = crow::HTTPMethod::POST;
        const std::string key1 = "2cb640c6-6995-4be5-9161-f0e2c1210501";
        const std::string key2 = "2cb640c6-6995-4be5-9161-f0e2c1210502";
        const auto payload = body(5, QStringLiteral("  充电方便，服务很好！  "));
        check(call(app, get, order.orderNo, {}).code == 401, "unauthenticated read denied");
        check(call(app, post, order.orderNo, {}, payload, key1).code == 401,
              "unauthenticated write denied");
        check(call(app, get, order.orderNo, other->accessToken).code == 404,
              "other user's review hidden");
        check(call(app, post, order.orderNo, other->accessToken, payload, key1).code == 404,
              "cannot review another user's order");
        check(data(call(app, get, order.orderNo, token))["review"].isNull(),
              "empty review is null");
        check(call(app, post, order.orderNo, token, payload).code == 400,
              "idempotency key required");
        for (const auto& invalid :
             {body(0, "x"), body(6, "x"), body(5, " \n\t"), body(5, QString(501, QChar(0x5145))),
              std::string("{\"rating\":1.5,\"content\":\"x\"}"),
              std::string("{\"rating\":5,\"content\":null}"),
              std::string("{\"rating\":5,\"content\":\"x\",\"userId\":1}")})
            check(call(app, post, order.orderNo, token, invalid, key1).code == 422,
                  "invalid review fields rejected");
        for (const int status : {40, 70, 80, 90})
        {
            auto invalid = order;
            invalid.status = status;
            repository.withTransaction([&] { repository.saveOrder(invalid); });
            check(reviews.submit(order.userId, order.orderNo, 5, "test", now).error ==
                      core::domain::ErrorCode::InvalidStateTransition,
                  "noncompleted order rejected");
        }
        auto unsettled = order;
        unsettled.settledAt.reset();
        repository.withTransaction([&] { repository.saveOrder(unsettled); });
        check(reviews.submit(order.userId, order.orderNo, 5, "test", now).error ==
                  core::domain::ErrorCode::InvalidStateTransition,
              "missing settlement rejected");
        repository.withTransaction([&] { repository.saveOrder(order); });
        const auto saved = call(app, post, order.orderNo, token, payload, key1);
        check(saved.code == 200 && data(saved)["rating"].toInt() == 5 &&
                  data(saved)["content"].toString() == QStringLiteral("充电方便，服务很好！"),
              "normalized review saved");
        check(data(call(app, post, order.orderNo, token, payload, key1)) == data(saved),
              "same idempotency key replays first result");
        check(data(call(app, post, order.orderNo, token, payload, key2)) == data(saved),
              "new key same review returns first result");
        check(call(app, post, order.orderNo, token, body(2, "changed"), key1).code == 409,
              "same key changed payload conflicts");
        check(reviews.submit(order.userId, order.orderNo, 2, "changed", now).error ==
                  core::domain::ErrorCode::AlreadyExists,
              "immutable once submitted");
        check(data(call(app, get, order.orderNo, token))["review"].toObject() == data(saved),
              "read returns submitted review");
        const auto unicodeOrder = completed[1];
        const auto emoji = QString::fromUtf8("\xf0\x9f\x94\x8b");
        const auto unicodeSession =
            sessions.issue("user:" + std::to_string(unicodeOrder.userId), "unicode",
                           TokenKind::User, {Role::User}, now, std::chrono::hours(1));
        check(call(app, post, unicodeOrder.orderNo, unicodeSession->accessToken,
                   body(1, emoji.repeated(501)), key1)
                      .code == 422,
              "501 emoji rejected");
        check(call(app, post, unicodeOrder.orderNo, unicodeSession->accessToken,
                   body(1, emoji.repeated(500)), key1)
                      .code == 200,
              "500 emoji accepted");
        const auto concurrentOrder = completed[2];
        std::atomic<int> successes{0};
        std::vector<std::thread> workers;
        for (int i = 0; i < 8; ++i)
            workers.emplace_back(
                [&, i]
                {
                    if (reviews
                            .submit(concurrentOrder.userId, concurrentOrder.orderNo, 4,
                                    "concurrent-" + std::to_string(i), now)
                            .ok())
                        ++successes;
                });
        for (auto& worker : workers)
            worker.join();
        check(successes == 1, "concurrent different reviews insert exactly once");
        const auto rollback = completed[3];
        try
        {
            repository.withTransaction(
                [&]
                {
                    repository.addOrderReview(
                        {rollback.orderNo, rollback.userId, 3, "rollback", 1});
                    throw std::runtime_error("injected failure");
                });
        }
        catch (const std::runtime_error&)
        {
        }
        check(!repository.orderReview(rollback.orderNo), "failed transaction rolls review back");

        // —— 场站评论墙（UC-U-12）：分组、倒序、脱敏、跨场站隔离 ——
        std::map<std::int64_t, std::vector<ChargingOrder>> completedByStation;
        for (const auto& candidate : completed)
            completedByStation[candidate.stationId].push_back(candidate);
        const auto wallOrderIt = std::find_if(completed.begin(), completed.end(),
                                              [&](const ChargingOrder& candidate) {
                                                  return candidate.userId != persistedUser &&
                                                         !repository.orderReview(candidate.orderNo);
                                              });
        check(wallOrderIt != completed.end(), "wall target order available");
        if (wallOrderIt == completed.end())
            return 1;
        const auto wallOrder = *wallOrderIt;
        const auto wallStation = wallOrder.stationId;
        persistedWallStation = wallStation;
        // 评价时间取 now+1h，保证是全场站最新一条，排序断言不受同刻时间戳影响。
        const auto submitted = reviews.submit(wallOrder.userId, wallOrder.orderNo, 5, "wall-review",
                                              now + std::chrono::hours(1));
        check(submitted.ok(), "wall setup review submitted");
        // 期望集合：该场站全部已完成订单的已有评价；作者规则与服务层一致（昵称优先，否则掩码手机号）。
        std::map<std::string, std::pair<int, std::string>> expected;
        for (const auto& candidate : completedByStation.at(wallStation))
            if (const auto existing = repository.orderReview(candidate.orderNo))
                expected[existing->content] = {
                    existing->rating, expectedAuthor(*repository.findById(existing->userId))};
        check(expected.count("wall-review") == 1, "wall expectation contains setup review");
        check(callStationWall(app, wallStation, {}).code == 401, "wall requires bearer token");
        check(callStationWall(app, 999999, token).code == 404, "unknown station not found");
        const auto items = data(callStationWall(app, wallStation, token))["items"].toArray();
        check(items.size() == static_cast<int>(expected.size()), "wall lists station reviews");
        auto remaining = expected;
        bool ordered = true;
        qint64 previousCreatedAt = std::numeric_limits<qint64>::max();
        QString wallReviewAuthor;
        for (const auto& value : items)
        {
            const auto item = value.toObject();
            const auto content = item["content"].toString().toStdString();
            const auto found = remaining.find(content);
            check(found != remaining.end(), "wall item belongs to the expected station reviews");
            if (found == remaining.end())
                continue;
            check(item["rating"].toInt() == found->second.first &&
                      item["author"].toString().toStdString() == found->second.second,
                  "wall rating and author match nickname or masked phone");
            remaining.erase(found);
            const auto createdAt = item["createdAt"].toVariant().toLongLong();
            if (previousCreatedAt < createdAt)
                ordered = false;
            previousCreatedAt = createdAt;
            if (content == "wall-review")
                wallReviewAuthor = item["author"].toString();
        }
        check(ordered, "wall sorted by createdAt desc");
        check(remaining.empty(), "wall lists every station review exactly once");
        check(!wallReviewAuthor.isEmpty(), "wall review author present");
        bool identityHidden = true;
        for (const auto& value : items)
        {
            const auto item = value.toObject();
            identityHidden = identityHidden && !item.contains("userId") &&
                             !item.contains("orderNo") && !item.contains("phone");
        }
        check(identityHidden, "wall hides internal identity fields");
        for (const auto& [otherStation, otherOrders] : completedByStation)
        {
            if (otherStation == wallStation)
                continue;
            for (const auto& value :
                 data(callStationWall(app, otherStation, token))["items"].toArray())
                check(value.toObject()["content"].toString() != QStringLiteral("wall-review"),
                      "wall keeps reviews inside their station");
            break;
        }
        const auto limited = reviews.stationReviews(wallStation, 1);
        check(limited.ok() && limited.value->size() == 1 &&
                  limited.value->front().content == "wall-review",
              "wall limit returns latest review first");
        // 昵称为空回退掩码手机号；注销后展示“已注销用户”。
        auto wallAccount = *repository.findById(wallOrder.userId);
        check(repository.updateNickname(wallOrder.userId, wallAccount.version, "", wallAccount) ==
                  AccountWriteResult::Success,
              "cleared nickname for mask fallback");
        const auto maskedAuthor = data(callStationWall(app, wallStation, token))["items"]
                                      .toArray()
                                      .at(0)
                                      .toObject()["author"]
                                      .toString()
                                      .toStdString();
        check(maskedAuthor == maskedPhone(repository.findById(wallOrder.userId)->phone),
              "empty nickname falls back to masked phone");
        repository.setActiveFlowFlag(wallOrder.userId, false);
        UserAccount anonymized;
        check(repository.anonymize(wallOrder.userId, anonymized) == AccountWriteResult::Success,
              "anonymized wall author");
        check(data(callStationWall(app, wallStation, token))["items"]
                      .toArray()
                      .at(0)
                      .toObject()["author"]
                      .toString() == QStringLiteral("已注销用户"),
              "anonymized author shown as placeholder");
    }
    {
        infrastructure::sqlite::SqliteRepository reopened(path);
        OrderReviewService reviews(reopened, reopened);
        const auto result = reviews.get(persistedUser, persistedOrder);
        check(result.ok() && result.value->has_value() && (*result.value)->rating == 5,
              "review survives repository restart");
        const auto wall = reviews.stationReviews(persistedWallStation, 200);
        check(wall.ok() && !wall.value->empty() &&
                  std::any_of(wall.value->begin(), wall.value->end(),
                              [](const StationReviewView& view)
                              { return view.content == "wall-review"; }),
              "station wall survives repository restart");
        check(reopened.check().ready(), "v9 readiness succeeds");
    }
    // Simulate a pre-feature v8 database in this test-only temporary directory.
    sqlite3* database = nullptr;
    check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "open migration fixture");
    check(sqlite3_exec(database,
                       "DROP TABLE order_review; DELETE FROM schema_version WHERE version=9;",
                       nullptr, nullptr, nullptr) == SQLITE_OK,
          "prepare v8 fixture");
    sqlite3_close(database);
    {
        infrastructure::sqlite::SqliteRepository upgraded(path);
        check(upgraded.check().ready() && upgraded.order(persistedOrder).has_value() &&
                  !upgraded.orderReview(persistedOrder),
              "v8 to v9 preserves orders and adds empty reviews");
    }
    if (failures == 0)
        std::cout << "Order review contracts, concurrency, rollback and persistence passed\n";
    return failures ? 1 : 0;
}
