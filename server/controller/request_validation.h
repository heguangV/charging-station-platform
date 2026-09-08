// 请求入参校验工具集：JSON
// 体解析（字段白名单/必填/大小上限）、字符串与整数字段校验、分页排序过滤白名单、查询参数与幂等键提取。
// controller 各路由共用的第一道入参防线；非法输入返回 nullopt
// 或诊断信息，由调用方映射为统一错误响应。
#pragma once

#include <crow.h>

#include <QJsonObject>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace ncs::server::controller
{

struct JsonValidationResult
{
    std::optional<QJsonObject> object;
    std::string diagnostic;
};

JsonValidationResult parseJsonObject(const crow::request& request,
                                     const std::unordered_set<std::string>& allowedFields,
                                     const std::unordered_set<std::string>& requiredFields = {},
                                     std::size_t maximumBytes = 1024 * 1024);
bool validStringField(const QJsonObject& object, std::string_view field, int minimumLength,
                      int maximumLength);
bool validIntegerField(const QJsonObject& object, std::string_view field, long long minimum,
                       long long maximum);
bool hasOnlyFields(const QJsonObject& object, const std::unordered_set<std::string>& allowedFields);

struct Pagination
{
    int page = 1;
    int pageSize = 20;
    std::string sort;
    std::unordered_map<std::string, std::string> filters;
};

std::optional<Pagination>
parsePagination(const crow::request& request, const std::unordered_set<std::string>& sortWhitelist,
                const std::unordered_set<std::string>& filterWhitelist = {});
std::optional<std::optional<std::int64_t>> parseIntegerFilter(const Pagination& pagination,
                                                              std::string_view name,
                                                              std::int64_t minimum,
                                                              std::int64_t maximum);

using QueryParameters = std::unordered_map<std::string, std::string>;

std::optional<QueryParameters>
parseQueryParameters(const crow::request& request,
                     const std::unordered_set<std::string>& allowedParameters);
std::optional<std::optional<std::int64_t>> parseIntegerParameter(const QueryParameters& parameters,
                                                                 std::string_view name,
                                                                 std::int64_t minimum,
                                                                 std::int64_t maximum);

std::optional<std::string_view> idempotencyKey(const crow::request& request);

} // namespace ncs::server::controller
