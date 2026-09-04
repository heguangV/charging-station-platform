#include "server/controller/health_routes.h"

#include "server/controller/api_response.h"
#include "server/middleware/authorization.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <asio/ip/address.hpp>

namespace ncs::server::controller {
namespace {

crow::response json(const int status, const QJsonObject &object)
{
    crow::response response;
    response.code = status;
    response.body = QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
    response.set_header("Content-Type", "application/json; charset=utf-8");
    applyPublicSecurityHeaders(response);
    return response;
}

bool isLoopback(const std::string_view remoteAddress)
{
    asio::error_code error;
    const auto address = asio::ip::make_address(remoteAddress, error);
    return !error && address.is_loopback();
}

} // namespace

HealthRoutes::HealthRoutes(
    ApiRoutes &routes,
    core::application::ReadinessProbe &readinessProbe,
    core::application::SessionManager &sessions)
{
    routes.route("/system/health/live")([] {
        return json(200, QJsonObject{{QStringLiteral("status"), QStringLiteral("UP")}});
    });

    routes.route("/system/health/ready")
        ([&readinessProbe, &sessions](const crow::request &request) {
            if (!isLoopback(request.remote_ip_address)) {
                const auto authorization = middleware::authorize(
                    request,
                    sessions,
                    {core::application::TokenKind::Administrator},
                    {core::application::Role::Operator, core::application::Role::Owner},
                    std::chrono::system_clock::now());
                if (!authorization.context) {
                    return errorResponse(
                        authorization.error,
                        authorization.error == core::domain::ErrorCode::Forbidden
                            ? "ready check is forbidden" : "authentication required",
                        "无权查看服务就绪状态");
                }
            }
            const core::application::ReadinessStatus status = readinessProbe.check();
            return json(status.ready() ? 200 : 503, QJsonObject{
                {QStringLiteral("status"), status.ready()
                     ? QStringLiteral("UP") : QStringLiteral("DOWN")},
                {QStringLiteral("checks"), QJsonObject{
                     {QStringLiteral("schemaVersion"), status.schemaVersion},
                     {QStringLiteral("databaseReadWrite"), status.databaseReadWrite},
                     {QStringLiteral("walEnabled"), status.walEnabled},
                     {QStringLiteral("migrationsComplete"), status.migrationsComplete},
                 }},
            });
        });
}

} // namespace ncs::server::controller
