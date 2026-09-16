#include "server/controller/agent_controller.h"

#include "core/domain/error_code.h"
#include "server/controller/api_response.h"
#include "server/controller/async_response.h"
#include "server/controller/request_validation.h"
#include "server/controller/station_dto.h"
#include "server/controller/user_auth.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <chrono>
#include <optional>
#include <string>

namespace ncs::server::controller
{
namespace
{

// 与 SRS 对齐：自然语言提问长度上限（Unicode 码点），避免超长文本占用大模型配额。
constexpr int maxMessageCodePoints = 600;
constexpr int maxRequestIdLength = 72;

QJsonObject poiJson(const agent::AgentPoi& poi)
{
    return {
        {QStringLiteral("id"), QString::fromStdString(poi.id)},
        {QStringLiteral("name"), QString::fromStdString(poi.name)},
        {QStringLiteral("category"), QString::fromStdString(poi.category)},
        {QStringLiteral("address"), QString::fromStdString(poi.address)},
        {QStringLiteral("tel"), QString::fromStdString(poi.tel)},
        {QStringLiteral("latitudeE6"), QJsonValue(static_cast<qint64>(poi.latitudeE6))},
        {QStringLiteral("longitudeE6"), QJsonValue(static_cast<qint64>(poi.longitudeE6))},
        {QStringLiteral("distanceMeter"), QJsonValue(static_cast<qint64>(poi.distanceMeter))},
    };
}

QJsonObject routeJson(const agent::AgentRoute& route)
{
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
    QJsonArray polyline;
    for (const auto& point : route.polyline)
    {
        polyline.append(QJsonObject{
            {QStringLiteral("latitudeE6"), QJsonValue(static_cast<qint64>(point.latitudeE6))},
            {QStringLiteral("longitudeE6"), QJsonValue(static_cast<qint64>(point.longitudeE6))},
        });
    }
    return {
        {QStringLiteral("destinationName"), QString::fromStdString(route.destinationName)},
        {QStringLiteral("originLatitudeE6"),
         QJsonValue(static_cast<qint64>(route.originLatitudeE6))},
        {QStringLiteral("originLongitudeE6"),
         QJsonValue(static_cast<qint64>(route.originLongitudeE6))},
        {QStringLiteral("destinationLatitudeE6"),
         QJsonValue(static_cast<qint64>(route.destinationLatitudeE6))},
        {QStringLiteral("destinationLongitudeE6"),
         QJsonValue(static_cast<qint64>(route.destinationLongitudeE6))},
        {QStringLiteral("distanceMeter"), QJsonValue(static_cast<qint64>(route.distanceMeter))},
        {QStringLiteral("durationSecond"), QJsonValue(static_cast<qint64>(route.durationSecond))},
        {QStringLiteral("provider"), QString::fromStdString(route.provider)},
        {QStringLiteral("fallback"), route.fallback},
        {QStringLiteral("browserUrl"), QString::fromStdString(route.browserUrl)},
        {QStringLiteral("steps"), steps},
        {QStringLiteral("polyline"), polyline},
    };
}

crow::response agentResponse(const agent::AgentResult& result)
{
    QJsonArray stations;
    for (const auto& station : result.stations)
        stations.append(stationJson(station));
    QJsonArray pois;
    for (const auto& poi : result.pois)
        pois.append(poiJson(poi));
    QJsonArray actions;
    for (const auto& action : result.actions)
    {
        actions.append(QJsonObject{
            {QStringLiteral("type"), QString::fromStdString(action.type)},
            {QStringLiteral("label"), QString::fromStdString(action.label)},
            {QStringLiteral("targetId"), QString::fromStdString(action.targetId)},
            {QStringLiteral("url"), QString::fromStdString(action.url)},
        });
    }
    QJsonArray tools;
    for (const auto& name : result.tools)
        tools.append(QString::fromStdString(name));

    // 结构化响应：前端据此直接渲染站点卡片、高亮地图 Marker、展示 POI 与导航按钮，
    // 因此 reply 之外必须带上 stations / pois / route / actions。
    return successResponse(QJsonObject{
        {QStringLiteral("reply"), QString::fromStdString(result.reply)},
        {QStringLiteral("stations"), stations},
        {QStringLiteral("pois"), pois},
        {QStringLiteral("route"),
         result.route ? QJsonValue(routeJson(*result.route)) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("actions"), actions},
        {QStringLiteral("tools"), tools},
        {QStringLiteral("llmUsed"), result.llmUsed},
        {QStringLiteral("degraded"), result.degraded},
    });
}

crow::response validationFailure()
{
    return errorResponse(core::domain::ErrorCode::ValidationFailed, "invalid agent request",
                         "提问内容或位置参数不符合要求");
}

// 解析并校验可选的 location 对象；返回 false 表示请求非法。
bool parseLocation(const QJsonObject& body, std::optional<std::int64_t>& latitudeE6,
                   std::optional<std::int64_t>& longitudeE6)
{
    const auto value = body.value(QStringLiteral("location"));
    if (value.isUndefined() || value.isNull())
        return true;
    if (!value.isObject())
        return false;
    const auto location = value.toObject();
    if (!hasOnlyFields(location, {"latitudeE6", "longitudeE6"}))
        return false;
    const auto latitude = location.value(QStringLiteral("latitudeE6"));
    const auto longitude = location.value(QStringLiteral("longitudeE6"));
    if (!latitude.isDouble() || !longitude.isDouble())
        return false;
    const auto latitudeValue = static_cast<std::int64_t>(latitude.toDouble());
    const auto longitudeValue = static_cast<std::int64_t>(longitude.toDouble());
    if (latitudeValue < -90000000 || latitudeValue > 90000000 || longitudeValue < -180000000 ||
        longitudeValue > 180000000)
    {
        return false;
    }
    // 经纬度必须成对出现，否则无法定位；缺失时走关键词地理编码或默认位置降级。
    if (latitudeValue == 0 && longitudeValue == 0)
        return false;
    latitudeE6 = latitudeValue;
    longitudeE6 = longitudeValue;
    return true;
}

} // namespace

AgentController::AgentController(ApiRoutes& routes, agent::AgentService& agent,
                                 core::application::SessionManager& sessions,
                                 core::application::BoundedExecutor& executor)
{
    routes.route("/user/agent/chat")
        .methods(crow::HTTPMethod::POST)(
            [&agent, &sessions, &executor](const crow::request& request, crow::response& response)
            {
                crow::response failure;
                const auto userId = requireUserId(request, sessions, failure);
                if (!userId)
                {
                    response = std::move(failure);
                    response.end();
                    return;
                }

                const auto parsed = parseJsonObject(
                    request, {"message", "location", "coordinateType", "chargerType"}, {"message"},
                    16 * 1024);
                if (!parsed.object)
                {
                    response = validationFailure();
                    response.end();
                    return;
                }

                const auto messageValue = parsed.object->value(QStringLiteral("message"));
                if (!messageValue.isString())
                {
                    response = validationFailure();
                    response.end();
                    return;
                }
                const auto message = messageValue.toString().trimmed();
                if (message.isEmpty() ||
                    static_cast<int>(message.toUcs4().size()) > maxMessageCodePoints ||
                    message.contains(QChar(0)))
                {
                    response = validationFailure();
                    response.end();
                    return;
                }

                std::optional<std::int64_t> latitudeE6;
                std::optional<std::int64_t> longitudeE6;
                if (!parseLocation(*parsed.object, latitudeE6, longitudeE6))
                {
                    response = validationFailure();
                    response.end();
                    return;
                }

                bool wgs84Location = false;
                const auto coordinateType = parsed.object->value(QStringLiteral("coordinateType"));
                if (coordinateType.isString())
                {
                    const auto type = coordinateType.toString();
                    if (type == QStringLiteral("wgs84"))
                        wgs84Location = true;
                    else if (type != QStringLiteral("gcj02"))
                    {
                        response = validationFailure();
                        response.end();
                        return;
                    }
                    // 坐标类型只在有坐标时才有意义；无坐标时忽略而不报错。
                }

                std::optional<int> chargerType;
                const auto chargerValue = parsed.object->value(QStringLiteral("chargerType"));
                if (!chargerValue.isUndefined())
                {
                    if (!chargerValue.isDouble() ||
                        (chargerValue.toInt(-1) != 0 && chargerValue.toInt(-1) != 1))
                    {
                        response = validationFailure();
                        response.end();
                        return;
                    }
                    chargerType = chargerValue.toInt();
                }

                const auto incomingRequestId = request.get_header_value("X-Request-ID");

                dispatchBlocking(request, response, executor,
                                 [&agent, userId = *userId, message = message.toStdString(),
                                  latitudeE6, longitudeE6, wgs84Location, chargerType,
                                  requestId = incomingRequestId.size() > maxRequestIdLength
                                                  ? std::string()
                                                  : incomingRequestId]()
                                 {
                                     agent::AgentContext context;
                                     // 用户身份只来自 Bearer 会话，绝不接受请求体中的用户标识。
                                     context.userId = userId;
                                     context.requestId = requestId;
                                     context.latitudeE6 = latitudeE6;
                                     context.longitudeE6 = longitudeE6;
                                     context.wgs84Location = wgs84Location;
                                     context.chargerType = chargerType;
                                     context.now = std::chrono::system_clock::now();
                                     return agentResponse(agent.chat(message, context));
                                 });
            });
}

} // namespace ncs::server::controller
