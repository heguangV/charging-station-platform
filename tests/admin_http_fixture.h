#pragma once
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <stdexcept>

inline void require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
struct AdminRequest
{
    QByteArray method;
    QUrl url;
    QJsonObject body;
    QHash<QByteArray, QByteArray> headers;
};
class AdminHttpFixture : public QTcpServer
{
  public:
    QList<AdminRequest> requests;
    bool expire = false, failUsers = false, emptyUsers = false, mustChange = false,
         wrongReauth = false;
    int userStatus = 1;
    int userVersion = 8;
    AdminHttpFixture()
    {
        require(listen(QHostAddress::LocalHost, 0), "fixture listen");
        connect(this, &QTcpServer::newConnection, this,
                [this]
                {
                    while (hasPendingConnections())
                    {
                        auto* socket = nextPendingConnection();
                        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                        connect(
                            socket, &QTcpSocket::readyRead, socket,
                            [this, socket]
                            {
                                QByteArray buffer =
                                    socket->property("buffer").toByteArray() + socket->readAll();
                                socket->setProperty("buffer", buffer);
                                const auto end = buffer.indexOf("\r\n\r\n");
                                if (end < 0 || socket->property("handled").toBool())
                                    return;
                                const auto lines = buffer.left(end).split('\n');
                                AdminRequest request;
                                auto first = lines.first().trimmed().split(' ');
                                request.method = first[0];
                                request.url = QUrl(QString::fromUtf8(first[1]));
                                for (int i = 1; i < lines.size(); ++i)
                                {
                                    auto line = lines[i].trimmed();
                                    auto colon = line.indexOf(':');
                                    if (colon > 0)
                                        request.headers.insert(line.left(colon).toLower(),
                                                               line.mid(colon + 1).trimmed());
                                }
                                const auto length = request.headers.value("content-length").toInt();
                                if (buffer.size() < end + 4 + length)
                                    return;
                                socket->setProperty("handled", true);
                                request.body =
                                    QJsonDocument::fromJson(buffer.mid(end + 4, length)).object();
                                requests.append(request);
                                int status = 200;
                                QJsonObject result = dispatch(request, status);
                                const auto bytes =
                                    QJsonDocument(result).toJson(QJsonDocument::Compact);
                                socket->write(
                                    "HTTP/1.1 " + QByteArray::number(status) +
                                    " Result\r\nContent-Type: application/json\r\nConnection: "
                                    "close\r\nContent-Length: " +
                                    QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
                                socket->disconnectFromHost();
                            });
                    }
                });
    }
    QUrl baseUrl() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/api/v1/").arg(serverPort()));
    }
    int count(const QString& path, const QByteArray& method = "GET") const
    {
        int result = 0;
        for (const auto& r : requests)
            if (r.url.path() == "/api/v1/" + path && r.method == method)
                ++result;
        return result;
    }

  private:
    QJsonObject dispatch(const AdminRequest& request, int& status)
    {
        const auto path = request.url.path().mid(QStringLiteral("/api/v1/").size());
        const QUrlQuery query(request.url);
        auto ok = [](QJsonObject data) {
            return QJsonObject{{"success", true}, {"code", 0}, {"data", data}};
        };
        auto error = [&status](int code)
        {
            status = code;
            return QJsonObject{{"success", false},
                               {"code", 10001},
                               {"userMessage", QStringLiteral("测试请求失败，请重试")}};
        };
        if (path == "admin/auth/login")
            return ok({{"accessToken", "fixture-session"},
                       {"admin", QJsonObject{{"username", "operator"},
                                             {"mustChangePassword", mustChange}}}});
        require(request.headers.value("authorization") == "Bearer fixture-session",
                "missing bearer header");
        if (expire)
            return error(401);
        if (request.method != "GET" && path != "admin/auth/reauth" && path != "admin/auth/logout" &&
            path != "admin/me/password")
            require(!request.headers.value("idempotency-key").isEmpty(), "missing idempotency key");
        if (query.hasQueryItem("pageSize"))
            require(query.queryItemValue("pageSize").toInt() <= 100, "invalid pagination size");
        if (path == "admin/auth/reauth")
        {
            if (wrongReauth)
                return error(401);
            require(request.body.value("password") == "fixture-password", "reauth password");
            return ok({{"reauthExpiresAt", QDateTime::currentSecsSinceEpoch() + 900}});
        }
        if (path == "admin/auth/logout")
            return ok({});
        if (path == "admin/me/password")
        {
            require(request.body.value("currentPassword") == "fixture-password",
                    "current password");
            require(request.body.value("newPassword") == "fixture-new-password", "new password");
            mustChange = false;
            return ok({{"mustChangePassword", false}});
        }
        if (path == "admin/stats/revenue")
        {
            require(query.queryItemValue("bucket") == "hour",
                    "UTC day buckets must not masquerade as Beijing days");
            return ok({{"totalAmountCent", 12345},
                       {"items", QJsonArray{QJsonObject{
                                     {"bucketStart", QDateTime::currentSecsSinceEpoch() - 3600},
                                     {"amountCent", 12345},
                                     {"orderCount", 9}}}}});
        }
        if (path == "admin/stats/charger-status")
            return ok({{"idleCount", 12},
                       {"occupiedCount", 6},
                       {"faultyCount", 2},
                       {"restartingCount", 1},
                       {"disabledCount", 3},
                       {"operationalCount", 21},
                       {"totalCount", 24},
                       {"healthPercent", 87.5}});
        if (path == "admin/stations")
            return ok(
                {{"total", 1},
                 {"items", QJsonArray{QJsonObject{{"id", 7},
                                                  {"code", "ZGC"},
                                                  {"name", QStringLiteral("中关村绿色能源站")},
                                                  {"adcode", "110108"},
                                                  {"enabled", true},
                                                  {"version", 3}}}}});
        if (path == "admin/chargers")
            return ok({{"total", 1},
                       {"items", QJsonArray{QJsonObject{{"id", 42},
                                                        {"stationId", 7},
                                                        {"code", "ZGC-C01"},
                                                        {"chargerType", 1},
                                                        {"powerWatt", 120000},
                                                        {"status", 1},
                                                        {"statusText", QStringLiteral("占用")},
                                                        {"totalCount", 157},
                                                        {"totalMinutes", 1234},
                                                        {"version", 5}}}}});
        if (path == "admin/users")
        {
            if (failUsers)
                return error(503);
            if (emptyUsers)
                return ok({{"total", 0}, {"items", QJsonArray{}}});
            return ok({{"total", 51},
                       {"items", QJsonArray{QJsonObject{{"id", 9},
                                                        {"nickname", QStringLiteral("绿色出行")},
                                                        {"phoneMasked", "138****8000"},
                                                        {"status", userStatus},
                                                        {"balanceCent", 5025},
                                                        {"registeredAt", 1704067200}}}}});
        }
        if (path == "admin/users/9")
            return ok({{"id", 9},
                       {"status", userStatus},
                       {"version", userVersion},
                       {"hasActiveFlow", true}});
        if (path == "admin/users/9/status")
        {
            require(request.body.value("version").toInt() == userVersion,
                    "must fetch user detail version");
            require(request.body.value("status").toInt() == 1 - userStatus,
                    "must toggle numeric user status");
            userStatus = 1 - userStatus;
            ++userVersion;
            return ok({{"activeFlowPreserved", true}});
        }
        if (path == "admin/chargers/42/restart-commands")
        {
            require(count("admin/auth/reauth", "POST") > 0, "restart before reauth");
            require(request.body.value("confirm").toBool(), "restart confirm missing");
            return ok({{"commandNo", "cmd-test"}, {"status", "PENDING"}});
        }
        if (path == "admin/device-commands/cmd-test")
            return ok({{"status", "SUCCEEDED"}});
        if (path == "admin/predictions")
            return ok({{"items", QJsonArray{QJsonObject{{"stationId", 7},
                                                        {"targetAt", 1704067200},
                                                        {"predictedEnergyMwh", 65400000},
                                                        {"predictedIdleCount", 8},
                                                        {"peakFlag", true},
                                                        {"staleFlag", true}}}}});
        if (path == "admin/ml-tasks")
        {
            require(request.body.value("taskType") == "PREDICT", "prediction task type");
            require(request.body.value("horizonHours").toArray() == QJsonArray{1, 6, 24},
                    "prediction horizons");
            status = 202;
            return ok({{"taskNo", "task-test"}, {"status", "PENDING"}});
        }
        if (path == "admin/ml-tasks/task-test")
            return ok({{"status", "SUCCEEDED"}});
        throw std::runtime_error("unexpected admin request");
    }
};
