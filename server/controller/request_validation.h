#pragma once

#include <crow.h>

#include <QJsonObject>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace ncs::server::controller {

struct JsonValidationResult {
  std::optional<QJsonObject> object;
  std::string diagnostic;
};

JsonValidationResult
parseJsonObject(const crow::request &request,
                const std::unordered_set<std::string> &allowedFields,
                const std::unordered_set<std::string> &requiredFields = {},
                std::size_t maximumBytes = 1024 * 1024);
bool validStringField(const QJsonObject &object, std::string_view field,
                      int minimumLength, int maximumLength);
bool validIntegerField(const QJsonObject &object, std::string_view field,
                       long long minimum, long long maximum);
bool hasOnlyFields(const QJsonObject &object,
                   const std::unordered_set<std::string> &allowedFields);

struct Pagination {
  int page = 1;
  int pageSize = 20;
  std::string sort;
  std::unordered_map<std::string, std::string> filters;
};

std::optional<Pagination>
parsePagination(const crow::request &request,
                const std::unordered_set<std::string> &sortWhitelist,
                const std::unordered_set<std::string> &filterWhitelist = {});
std::optional<std::optional<std::int64_t>>
parseIntegerFilter(const Pagination &pagination, std::string_view name,
                   std::int64_t minimum, std::int64_t maximum);

using QueryParameters = std::unordered_map<std::string, std::string>;

std::optional<QueryParameters>
parseQueryParameters(const crow::request &request,
                     const std::unordered_set<std::string> &allowedParameters);
std::optional<std::optional<std::int64_t>>
parseIntegerParameter(const QueryParameters &parameters, std::string_view name,
                      std::int64_t minimum, std::int64_t maximum);

std::optional<std::string_view> idempotencyKey(const crow::request &request);

} // namespace ncs::server::controller
