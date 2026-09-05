#include "server/controller/navigation_routes.h"

#include "core/domain/error_code.h"
#include "server/controller/api_response.h"
#include "server/controller/async_response.h"
#include "server/controller/request_validation.h"
#include "server/controller/user_auth.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

namespace ncs::server::controller
{
namespace
{

std::optional<core::application::TravelMode> travelMode(const std::string& value)
{
    if (value == "driving")
        return core::application::TravelMode::Driving;
    if (value == "walking")
        return core::application::TravelMode::Walking;
    if (value == "transit")
        return core::application::TravelMode::Transit;
    return std::nullopt;
}

QString browserRouteUrl(const core::application::NavigationResult& route)
{
    QString type;
    switch (route.mode)
    {
    case core::application::TravelMode::Driving:
        type = QStringLiteral("drive");
        break;
    case core::application::TravelMode::Walking:
        type = QStringLiteral("walk");
        break;
    case core::application::TravelMode::Transit:
        type = QStringLiteral("bus");
        break;
    }
    const auto coordinate = [](const core::application::RoutePoint point)
    {
        return QStringLiteral("%1,%2")
            .arg(point.latitudeE6 / 1e6, 0, 'f', 6)
            .arg(point.longitudeE6 / 1e6, 0, 'f', 6);
    };
    QUrl url(QStringLiteral("https://apis.map.qq.com/uri/v1/routeplan"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("type"), type);
    query.addQueryItem(QStringLiteral("from"), QStringLiteral("当前位置"));
    query.addQueryItem(QStringLiteral("fromcoord"), coordinate(route.origin));
    query.addQueryItem(QStringLiteral("to"), QString::fromStdString(route.stationName));
    query.addQueryItem(QStringLiteral("tocoord"), coordinate(route.destination));
    query.addQueryItem(QStringLiteral("referer"), QStringLiteral("NCS"));
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

crow::response responseFor(const core::application::NavigationResult& route)
{
    QJsonArray polyline;
    for (const auto point : route.polyline)
    {
        polyline.append(QJsonObject{
            {QStringLiteral("latitudeE6"), QJsonValue(static_cast<qint64>(point.latitudeE6))},
            {QStringLiteral("longitudeE6"), QJsonValue(static_cast<qint64>(point.longitudeE6))},
        });
    }
    QJsonArray steps;
    for (const auto& step : route.steps)
    {
        steps.append(QJsonObject{
            {QStringLiteral("instruction"), QString::fromStdString(step.instruction)},
            {QStringLiteral("distanceMeter"), QJsonValue(static_cast<qint64>(step.distanceMeter))},
            {QStringLiteral("durationSecond"),
             QJsonValue(static_cast<qint64>(step.durationSecond))},
        });
    }
    const auto mode = core::application::travelModeName(route.mode);
    return successResponse(QJsonObject{
        {QStringLiteral("stationId"), QJsonValue(static_cast<qint64>(route.stationId))},
        {QStringLiteral("stationName"), QString::fromStdString(route.stationName)},
        {QStringLiteral("destinationAddress"), QString::fromStdString(route.destinationAddress)},
        {QStringLiteral("mode"), QString::fromLatin1(mode.data(), mode.size())},
        {QStringLiteral("originLatitudeE6"),
         QJsonValue(static_cast<qint64>(route.origin.latitudeE6))},
        {QStringLiteral("originLongitudeE6"),
         QJsonValue(static_cast<qint64>(route.origin.longitudeE6))},
        {QStringLiteral("destinationLatitudeE6"),
         QJsonValue(static_cast<qint64>(route.destination.latitudeE6))},
        {QStringLiteral("destinationLongitudeE6"),
         QJsonValue(static_cast<qint64>(route.destination.longitudeE6))},
        {QStringLiteral("distanceMeter"), QJsonValue(static_cast<qint64>(route.distanceMeter))},
        {QStringLiteral("durationSecond"), QJsonValue(static_cast<qint64>(route.durationSecond))},
        {QStringLiteral("provider"),
         route.routeFallback ? QStringLiteral("LOCAL_FALLBACK") : QStringLiteral("TENCENT_MAP")},
        {QStringLiteral("locationFallback"), route.locationFallback},
        {QStringLiteral("routeFallback"), route.routeFallback},
        {QStringLiteral("polyline"), polyline},
        {QStringLiteral("steps"), steps},
        {QStringLiteral("browserUrl"), browserRouteUrl(route)},
    });
}

} // namespace

NavigationRoutes::NavigationRoutes(ApiRoutes& routes,
                                   core::application::NavigationService& navigation,
                                   core::application::SessionManager& sessions,
                                   core::application::BoundedExecutor& executor)
{
    routes.route("/user/stations/<int>/route")
        .methods(crow::HTTPMethod::GET)(
            [&navigation, &sessions, &executor](const crow::request& request,
                                                crow::response& response, std::int64_t stationId)
            {
                crow::response failure;
                if (!requireUserId(request, sessions, failure))
                {
                    response = std::move(failure);
                    response.end();
                    return;
                }
                const auto parameters =
                    parseQueryParameters(request, {"latitudeE6", "longitudeE6", "keyword", "mode"});
                if (!parameters)
                {
                    response = errorResponse(core::domain::ErrorCode::ValidationFailed,
                                             "unsupported or duplicate route parameter",
                                             "导航参数不符合要求");
                    response.end();
                    return;
                }
                const auto latitude =
                    parseIntegerParameter(*parameters, "latitudeE6", -90000000, 90000000);
                const auto longitude =
                    parseIntegerParameter(*parameters, "longitudeE6", -180000000, 180000000);
                const auto modeValue = parameters->find("mode");
                const auto mode = modeValue == parameters->end()
                                      ? std::optional<core::application::TravelMode>{}
                                      : travelMode(modeValue->second);
                const auto keyword = parameters->find("keyword");
                if (!latitude || !longitude || (latitude->has_value() != longitude->has_value()) ||
                    !mode)
                {
                    response = errorResponse(core::domain::ErrorCode::ValidationFailed,
                                             "invalid route origin or travel mode",
                                             "位置或出行方式不符合要求");
                    response.end();
                    return;
                }
                if (keyword != parameters->end() && keyword->second.size() > 600)
                {
                    response = errorResponse(core::domain::ErrorCode::ValidationFailed,
                                             "route keyword is too long", "定位地址过长");
                    response.end();
                    return;
                }
                dispatchBlocking(
                    request, response, executor,
                    [&navigation, stationId, latitude, longitude, mode,
                     keyword = keyword == parameters->end() ? std::string() : keyword->second]()
                    {
                        const auto result = navigation.routeToStation(stationId, *latitude,
                                                                      *longitude, keyword, *mode);
                        if (!result.ok())
                        {
                            return errorResponse(result.error, "station not found",
                                                 "未找到相关电站");
                        }
                        return responseFor(*result.value);
                    });
            });
}

} // namespace ncs::server::controller
