// UC-U-09: real SQLite and HTTP authorization, rollback, concurrency and restart.
#include "core/application/charge_flow_service.h"
#include "core/application/order_review_service.h"
#include "infrastructure/sqlite/sqlite_repository.h"
#include "server/controller/flow_routes.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <sqlite3.h>
#include <atomic>
#include <iostream>
#include <thread>
using namespace ncs;
using namespace core::application;
namespace {
int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void sql(const std::string& path, const std::string& query) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) throw std::runtime_error("test database open failed");
    const int rc = sqlite3_exec(db, query.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (rc != SQLITE_OK) throw std::runtime_error("test SQL failed");
}
crow::response call(server::ServerApp& app, crow::HTTPMethod method, const std::string& path,
    const std::string& token, const std::string& body = "{}", const std::string& key = "9cb640c6-6995-4be5-9161-f0e2c1210501") {
    const auto url = "/api/v1" + path;
    crow::request req(method, url, url.substr(0, url.find('?')), crow::query_string(url), {}, body, 1, 1, true, false, false);
    req.remote_ip_address = "127.0.0.1";
    req.add_header("Content-Type", "application/json");
    if (!token.empty()) req.add_header("Authorization", "Bearer " + token);
    if (!key.empty()) req.add_header("Idempotency-Key", key);
    crow::response response; app.handle_full(req, response); return response;
}
QJsonObject data(const crow::response& response) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(response.body)).object()["data"].toObject();
}
}
int main(int argc, char** argv) {
    QCoreApplication appQt(argc, argv);
    QTemporaryDir temp;
    const auto path = temp.filePath("payment.db").toStdString();
    const auto now = std::chrono::system_clock::now();
    const auto second = std::chrono::seconds(1);
    std::string cancelledOrder;
    std::int64_t userId = 0;
    {
        infrastructure::sqlite::SqliteRepository repository(path);
        BusinessNumbers numbers(&repository);
        ChargeFlowService flows(repository, repository, repository, numbers, 60);
        WalletService wallet(repository, repository, numbers);
        OrderReviewService reviews(repository, repository);
        UserAccount user;
        user.username = "payment_owner"; user.phone = "13800228888"; user.nickname = "确认测试";
        user.registeredAt = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        check(repository.create(user) == AccountWriteResult::Success, "create owner");
        userId = user.id;
        check(wallet.recharge(userId, 10000, now).ok(), "fund owner");
        auto start = [&]() {
            auto flow = flows.createFlow(userId, 1, 1, std::nullopt, now);
            if (!flow.ok()) throw std::runtime_error("create flow failed");
            auto quote = flows.confirmQuote(userId, flow.value->flowNo, flow.value->quote->quoteNo, flow.value->version, now);
            if (!quote.ok()) throw std::runtime_error("confirm quote failed");
            auto charge = flows.start(userId, flow.value->flowNo, quote.value->version, std::nullopt, std::nullopt, now);
            if (!charge.ok()) throw std::runtime_error("start failed");
            return *charge.value;
        };
        const auto charged = start();
        const auto stopped = flows.settle(userId, charged.flowNo, charged.version, "USER_STOPPED", now + 60 * second);
        check(stopped.ok() && stopped.value->status == 100 && stopped.value->settledAt == 0 && stopped.value->paidCent == 0, "stop returns unpaid order");
        if (!stopped.ok()) return 1;
        const auto orderNo = stopped.value->orderNo;
        const auto snapshot = *repository.order(orderNo);
        check(wallet.overview(userId).balanceCent == 10000 && wallet.overview(userId).debtCent == 0, "stop does not change wallet");
        check(repository.charger(snapshot.chargerId)->status == ChargerStatus::Idle, "stop releases charger");
        check(flows.createFlow(userId, 1, 1, std::nullopt, now).error == core::domain::ErrorCode::ActiveFlowExists, "pending order blocks a second user flow");
        check(reviews.submit(userId, orderNo, 5, "pending", now).error == core::domain::ErrorCode::InvalidStateTransition, "pending cannot review");
        check(flows.confirmOrder(userId + 1000, orderNo, now).error == core::domain::ErrorCode::NotFound, "foreign confirmation hidden");
        sql(path, "CREATE TRIGGER fail_payment BEFORE INSERT ON wallet_transaction WHEN NEW.type=1 BEGIN SELECT RAISE(ABORT,'test rollback'); END");
        check(flows.confirmOrder(userId, orderNo, now + 100 * second).error == core::domain::ErrorCode::TransactionFailed, "payment failure is reported");
        check(repository.order(orderNo)->status == 100 && wallet.overview(userId).balanceCent == 10000, "payment failure rolls back order and wallet");
        sql(path, "DROP TRIGGER fail_payment");
        std::atomic<int> ok{0};
        std::thread first([&] { if (flows.confirmOrder(userId, orderNo, now + 1000 * second).ok()) ++ok; });
        std::thread secondThread([&] { if (flows.confirmOrder(userId, orderNo, now + 1000 * second).ok()) ++ok; });
        first.join(); secondThread.join();
        const auto paid = *repository.order(orderNo);
        check(ok == 2 && paid.status == 60 && paid.amountCent == snapshot.amountCent && paid.endedAt == snapshot.endedAt, "concurrent confirmation preserves frozen bill");
        check(wallet.overview(userId).balanceCent == 10000 - paid.amountCent, "only one deduction");
        check(wallet.transactions(userId, "CHARGE", 0, 0, 1, 20).value->total == 1, "one payment ledger entry");
        check(reviews.submit(userId, orderNo, 5, "满意", now).ok(), "confirmed payment connects to existing reviews");
        check(flows.appealOrder(userId, orderNo, "late", now).error == core::domain::ErrorCode::InvalidStateTransition, "paid order cannot appeal");
        check(wallet.recharge(userId, 10000, now).ok(), "fund next test");
        const auto charged2 = start();
        const auto stopped2 = flows.adminControlledSettle(charged2.flowNo, "设备维护", 0, now + 30 * second);
        check(stopped2.ok() && stopped2.value->status == 100, "admin stop also waits for user payment");
        if (!stopped2.ok()) return 1;
        cancelledOrder = stopped2.value->orderNo;
        const auto before = wallet.overview(userId);
        SessionManager sessions;
        const auto token = sessions.issue("user:" + std::to_string(userId), "payment", TokenKind::User, {Role::User}, now, std::chrono::hours(1))->accessToken;
        const auto foreign = sessions.issue("user:999999", "foreign", TokenKind::User, {Role::User}, now, std::chrono::hours(1))->accessToken;
        const auto admin = sessions.issue("admin:1", "admin", TokenKind::Administrator, {Role::Operator}, now, std::chrono::hours(1))->accessToken;
        BoundedExecutor executor(2, 16);
        IdempotencyService idempotency(&repository);
        server::ServerApp app;
        server::controller::ApiRoutes api(app);
        server::controller::FlowRoutes routes(api, flows, sessions, executor, idempotency);
        app.validate();
        const auto post = crow::HTTPMethod::POST; const auto get = crow::HTTPMethod::GET;
        const auto prefix = "/user/orders/" + cancelledOrder;
        check(call(app, post, prefix + "/confirmation", {}).code == 401, "confirmation needs login");
        check(call(app, post, prefix + "/confirmation", foreign).code == 404, "HTTP ownership enforced");
        check(call(app, post, prefix + "/appeals", token, R"({"reason":"  "})").code == 422, "blank appeal rejected");
        check(call(app, post, prefix + "/appeals", token, R"({"reason":"未达到预期电量"})", {}).code == 400, "appeal requires idempotency key");
        const auto appeal = call(app, post, prefix + "/appeals", token, R"({"reason":"  未达到预期电量  "})");
        check(appeal.code == 200 && data(appeal)["status"].toInt() == 110, "appeal submitted through HTTP");
        check(repository.order(cancelledOrder)->appealReason == "未达到预期电量", "trimmed reason persisted");
        check(call(app, post, prefix + "/appeals", token, R"({"reason":"  未达到预期电量  "})").body == appeal.body, "same key replays appeal");
        check(flows.appealOrder(userId, cancelledOrder, "different", now).error == core::domain::ErrorCode::AlreadyExists, "different appeal conflicts");
        check(call(app, post, prefix + "/confirmation", token).code == 409, "appeal blocks payment");
        check(call(app, get, "/admin/order-appeals", token).code == 403, "user cannot read admin appeal list");
        const auto list = call(app, get, "/admin/order-appeals?page=1&pageSize=20", admin);
        check(list.code == 200 && data(list)["items"].toArray().size() == 1 && data(list)["items"].toArray()[0].toObject()["reason"].toString() == QStringLiteral("未达到预期电量"), "admin receives reason");
        const auto approvalPath = "/admin/order-appeals/" + cancelledOrder + "/approval";
        check(call(app, post, approvalPath, token, R"({"confirmed":true})").code == 403, "user cannot approve appeal");
        check(call(app, post, approvalPath, admin, R"({"confirmed":false})").code == 422, "explicit approval required");
        // Reopen before approval to demonstrate pending reasons survive restarts.
        {
            infrastructure::sqlite::SqliteRepository reopened(path);
            check(reopened.order(cancelledOrder)->status == 110 && reopened.order(cancelledOrder)->appealReason == "未达到预期电量", "pending appeal survives restart");
        }
        sql(path, "CREATE TRIGGER fail_approval BEFORE UPDATE OF status ON charging_flow WHEN NEW.status=70 BEGIN SELECT RAISE(ABORT,'test approval'); END");
        check(flows.approveAppeal(1, cancelledOrder, now).error == core::domain::ErrorCode::TransactionFailed && repository.order(cancelledOrder)->status == 110, "approval failure fully rolls back");
        sql(path, "DROP TRIGGER fail_approval");
        const auto approved = call(app, post, approvalPath, admin, R"({"confirmed":true})");
        check(approved.code == 200 && data(approved)["status"].toInt() == 70, "admin cancels approved appeal");
        check(call(app, post, approvalPath, admin, R"({"confirmed":true})").body == approved.body, "approval replay is stable");
        check(wallet.overview(userId).balanceCent == before.balanceCent && wallet.overview(userId).debtCent == before.debtCent, "appeal and approval never charge or add debt");
        check(wallet.transactions(userId, "CHARGE", 0, 0, 1, 20).value->total == 1, "cancelled appeal creates no charge ledger");
        check(!flows.activeFlow(userId, now).hasActiveFlow && !repository.order(cancelledOrder)->settledAt, "cancel releases user but keeps order unpaid");
        check(reviews.submit(userId, cancelledOrder, 5, "cancelled", now).error == core::domain::ErrorCode::InvalidStateTransition, "cancelled order cannot review");
        check(call(app, post, prefix + "/confirmation", token, "{}", "9cb640c6-6995-4be5-9161-f0e2c1210502").code == 409, "cancelled order cannot pay");
    }
    {
        infrastructure::sqlite::SqliteRepository reopened(path);
        const auto order = reopened.order(cancelledOrder);
        check(order && order->status == 70 && order->reviewedBy == 1 && order->reviewedAt && !order->settledAt, "cancellation audit survives restart");
        check(reopened.check().ready(), "v10 database readiness");
    }
    std::cout << "payment checks failures=" << failures << '\n';
    return failures ? 1 : 0;
}
