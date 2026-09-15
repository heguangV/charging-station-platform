// 站点 DTO 序列化：站点列表 / 站点详情 / Agent 结构化响应对 station_summary 采用同一字段集，
// 由本头文件统一定义，避免各控制器各写一份而漂移。
// 契约：字段与 docs/database-api.md §4.1 一致；金额为整数分/千瓦时，坐标与距离为整数
// （E6 度、米），单位换算只在展示层进行。
// TODO: station_routes.cpp 仍保留一份等价的局部实现。该文件存在其他历史格式债，整文件
// clang-format 会产生与本任务无关的大量改写，因此待专门的格式整理任务中再改为引用本头文件。
#pragma once

#include "core/application/station_service.h"

#include <QJsonObject>
#include <QJsonValue>

namespace ncs::server::controller
{

inline QJsonObject stationJson(const core::application::StationSummary& summary)
{
    return {
        {QStringLiteral("id"), QJsonValue(static_cast<qint64>(summary.id))},
        {QStringLiteral("code"), QString::fromStdString(summary.code)},
        {QStringLiteral("name"), QString::fromStdString(summary.name)},
        {QStringLiteral("address"), QString::fromStdString(summary.address)},
        {QStringLiteral("adcode"), QString::fromStdString(summary.adcode)},
        {QStringLiteral("latitudeE6"), QJsonValue(static_cast<qint64>(summary.latitudeE6))},
        {QStringLiteral("longitudeE6"), QJsonValue(static_cast<qint64>(summary.longitudeE6))},
        {QStringLiteral("electricityPriceCentPerKwh"), summary.electricityPriceCentPerKwh},
        {QStringLiteral("servicePriceCentPerKwh"), summary.servicePriceCentPerKwh},
        {QStringLiteral("totalPriceCentPerKwh"), summary.totalPriceCentPerKwh},
        {QStringLiteral("idleCount"), summary.idleCount},
        {QStringLiteral("operationalCount"), summary.operationalCount},
        {QStringLiteral("totalCount"), summary.totalCount},
        {QStringLiteral("distanceMeter"), QJsonValue(static_cast<qint64>(summary.distanceMeter))},
    };
}

} // namespace ncs::server::controller
