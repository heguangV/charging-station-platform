#include "infrastructure/map/poi_service.h"

#include "core/application/station_service.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QString>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace ncs::infrastructure::map
{
namespace
{

constexpr const char* searchPath = "/ws/place/v1/search";
constexpr int maxRadiusMeter = 10000;
constexpr int maxLimit = 20;

std::int64_t e6FromCoordinate(const QJsonObject& location, const char* key)
{
    const auto value = location.value(QLatin1String(key));
    if (!value.isDouble())
        return 0;
    return static_cast<std::int64_t>(value.toDouble() * 1e6);
}

QString decimalCoordinate(const std::int64_t valueE6)
{
    return QString::number(static_cast<double>(valueE6) / 1e6, 'f', 6);
}

std::string text(const QJsonObject& object, const char* key)
{
    return object.value(QLatin1String(key)).toString().toStdString();
}

} // namespace

TencentPoiService::TencentPoiService(TencentMapClient client) : client_(std::move(client)) {}

bool TencentPoiService::available() const
{
    return client_.configured();
}

std::string TencentPoiService::keywordForCategory(const std::string& category)
{
    if (category == "餐饮" || category == "餐厅" || category == "吃饭" || category == "美食")
        return "餐厅";
    if (category == "咖啡" || category == "咖啡厅" || category == "咖啡店")
        return "咖啡厅";
    if (category == "便利店")
        return "便利店";
    if (category == "商场" || category == "购物")
        return "购物中心";
    return {};
}

std::vector<core::application::PoiItem> TencentPoiService::parseResponse(const QJsonObject& root,
                                                                         const int limit)
{
    std::vector<core::application::PoiItem> items;
    const auto data = root.value(QStringLiteral("data")).toArray();
    for (const auto& entry : data)
    {
        if (static_cast<int>(items.size()) >= limit)
            break;
        const auto object = entry.toObject();
        const auto location = object.value(QStringLiteral("location")).toObject();
        core::application::PoiItem item;
        item.latitudeE6 = e6FromCoordinate(location, "lat");
        item.longitudeE6 = e6FromCoordinate(location, "lng");
        // 无坐标的条目无法在 Web 地图上高亮，直接丢弃而不是展示一个不可定位的卡片。
        if (item.latitudeE6 == 0 && item.longitudeE6 == 0)
            continue;
        item.id = text(object, "id");
        item.name = text(object, "title");
        item.category = text(object, "category");
        item.address = text(object, "address");
        item.tel = text(object, "tel");
        if (item.tel.empty())
            item.tel = text(object, "tel_1");
        item.distanceMeter =
            static_cast<std::int64_t>(object.value(QStringLiteral("_distance")).toDouble(0));
        if (item.name.empty())
            continue;
        items.push_back(std::move(item));
    }
    return items;
}

core::application::ServiceResult<std::vector<core::application::PoiItem>>
TencentPoiService::search(const core::application::PoiQuery& query)
{
    using core::domain::ErrorCode;

    if (!available())
        return {ErrorCode::ExternalServiceUnavailable, std::nullopt};
    if (query.latitudeE6 < -90000000 || query.latitudeE6 > 90000000 ||
        query.longitudeE6 < -180000000 || query.longitudeE6 > 180000000 ||
        (query.latitudeE6 == 0 && query.longitudeE6 == 0))
    {
        return {ErrorCode::ValidationFailed, std::nullopt};
    }

    auto sortKeyword = keywordForCategory(query.category);
    if (!query.keyword.empty())
        sortKeyword = query.keyword;
    if (sortKeyword.empty())
        sortKeyword = "餐厅";
    const auto radius = std::clamp(query.radiusMeter, 100, maxRadiusMeter);
    const auto limit = std::clamp(query.limit, 1, maxLimit);

    QUrlQuery parameters;
    parameters.addQueryItem(QStringLiteral("keyword"), QString::fromStdString(sortKeyword));
    parameters.addQueryItem(
        QStringLiteral("boundary"),
        QStringLiteral("nearby(%1,%2,%3)")
            .arg(decimalCoordinate(query.latitudeE6), decimalCoordinate(query.longitudeE6))
            .arg(radius));
    parameters.addQueryItem(QStringLiteral("page_size"), QString::number(limit));
    parameters.addQueryItem(QStringLiteral("page_index"), QStringLiteral("1"));
    parameters.addQueryItem(QStringLiteral("orderby"), QStringLiteral("_distance"));

    const auto root = client_.get(QString::fromLatin1(searchPath), parameters);
    if (!root)
        return {ErrorCode::ExternalServiceUnavailable, std::nullopt};

    auto items = parseResponse(*root, limit);
    // 腾讯偶尔不返回 _distance：用 Haversine 补齐，保证前端距离展示不为 0。
    for (auto& item : items)
    {
        if (item.distanceMeter <= 0)
        {
            item.distanceMeter = core::application::StationService::haversineMeter(
                query.latitudeE6, query.longitudeE6, item.latitudeE6, item.longitudeE6);
        }
    }
    return {core::domain::ErrorCode::Ok, std::move(items)};
}

} // namespace ncs::infrastructure::map
