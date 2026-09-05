#include "server/controller/request_validation.h"

#include "core/application/idempotency_service.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QString>

#include <charconv>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace ncs::server::controller {
namespace {

std::optional<int> queryInteger(const crow::request &request, const char *name,
                                const int fallback) {
  const char *value = request.url_params.get(name);
  if (!value || *value == '\0')
    return fallback;
  int parsed = 0;
  const std::string_view text(value);
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    return std::nullopt;
  return parsed;
}

} // namespace

JsonValidationResult
parseJsonObject(const crow::request &request,
                const std::unordered_set<std::string> &allowedFields,
                const std::unordered_set<std::string> &requiredFields,
                const std::size_t maximumBytes) {
  if (request.body.size() > maximumBytes)
    return {std::nullopt, "request body too large"};
  const std::string &contentType = request.get_header_value("Content-Type");
  if (contentType.rfind("application/json", 0) != 0) {
    return {std::nullopt, "content type must be application/json"};
  }
  QJsonParseError error;
  const QJsonDocument document =
      QJsonDocument::fromJson(QByteArray::fromStdString(request.body), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    return {std::nullopt, "body must be a JSON object"};
  }
  const QJsonObject object = document.object();
  for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
    if (allowedFields.find(iterator.key().toStdString()) ==
        allowedFields.end()) {
      return {std::nullopt, "unknown JSON field"};
    }
  }
  for (const auto &required : requiredFields) {
    if (!object.contains(QString::fromStdString(required))) {
      return {std::nullopt, "required JSON field is missing"};
    }
  }
  return {object, {}};
}

bool validStringField(const QJsonObject &object, const std::string_view field,
                      const int minimumLength, const int maximumLength) {
  const QJsonValue value =
      object.value(QString::fromUtf8(field.data(), field.size()));
  if (!value.isString())
    return false;
  const qsizetype size = value.toString().size();
  return size >= minimumLength && size <= maximumLength;
}

bool validIntegerField(const QJsonObject &object, const std::string_view field,
                       const long long minimum, const long long maximum) {
  const QJsonValue value =
      object.value(QString::fromUtf8(field.data(), field.size()));
  if (!value.isDouble())
    return false;
  const double number = value.toDouble();
  if (!std::isfinite(number) ||
      number < static_cast<double>(std::numeric_limits<long long>::lowest()) ||
      number > static_cast<double>(std::numeric_limits<long long>::max()))
    return false;
  const auto integer = static_cast<long long>(number);
  return number == static_cast<double>(integer) && integer >= minimum &&
         integer <= maximum;
}

bool hasOnlyFields(
    const QJsonObject &object,
    const std::unordered_set<std::string> &allowedFields) {
  for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
    if (allowedFields.find(iterator.key().toStdString()) == allowedFields.end())
      return false;
  }
  return true;
}

std::optional<Pagination>
parsePagination(const crow::request &request,
                const std::unordered_set<std::string> &sortWhitelist,
                const std::unordered_set<std::string> &filterWhitelist) {
  const auto page = queryInteger(request, "page", 1);
  const auto pageSize = queryInteger(request, "pageSize", 20);
  if (!page || !pageSize || *page < 1 || *pageSize < 1 || *pageSize > 100) {
    return std::nullopt;
  }
  const char *sortValue = request.url_params.get("sort");
  const std::string sort = sortValue ? sortValue : "";
  if (!sort.empty() && sortWhitelist.find(sort) == sortWhitelist.end()) {
    return std::nullopt;
  }
  std::unordered_map<std::string, std::string> filters;
  std::unordered_set<std::string> seen;
  const auto question = request.raw_url.find('?');
  if (question != std::string::npos) {
    std::string_view query(request.raw_url.data() + question + 1,
                           request.raw_url.size() - question - 1);
    while (!query.empty()) {
      const auto separator = query.find('&');
      const std::string_view pair = query.substr(0, separator);
      const auto equals = pair.find('=');
      const std::string key(pair.substr(0, equals));
      if (key.empty() || !seen.insert(key).second)
        return std::nullopt;
      if (key != "page" && key != "pageSize" && key != "sort") {
        if (filterWhitelist.find(key) == filterWhitelist.end())
          return std::nullopt;
        const char *value = request.url_params.get(key.c_str());
        filters.emplace(key, value ? value : "");
      }
      if (separator == std::string_view::npos)
        break;
      query.remove_prefix(separator + 1);
    }
  }
  return Pagination{*page, *pageSize, sort, std::move(filters)};
}

std::optional<std::optional<std::int64_t>>
parseIntegerFilter(const Pagination &pagination, const std::string_view name,
                   const std::int64_t minimum, const std::int64_t maximum) {
  const auto found = pagination.filters.find(std::string(name));
  if (found == pagination.filters.end()) {
    return std::optional<std::int64_t>{};
  }
  std::int64_t parsed = 0;
  const std::string_view text(found->second);
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (text.empty() || result.ec != std::errc{} ||
      result.ptr != text.data() + text.size() || parsed < minimum ||
      parsed > maximum) {
    return std::nullopt;
  }
  return std::optional<std::int64_t>{parsed};
}

std::optional<QueryParameters>
parseQueryParameters(
    const crow::request &request,
    const std::unordered_set<std::string> &allowedParameters) {
  QueryParameters parameters;
  std::unordered_set<std::string> seen;
  const auto question = request.raw_url.find('?');
  if (question == std::string::npos)
    return parameters;

  std::string_view query(request.raw_url.data() + question + 1,
                         request.raw_url.size() - question - 1);
  while (!query.empty()) {
    const auto separator = query.find('&');
    const std::string_view pair = query.substr(0, separator);
    const auto equals = pair.find('=');
    const std::string key(pair.substr(0, equals));
    if (key.empty() || !seen.insert(key).second ||
        allowedParameters.find(key) == allowedParameters.end()) {
      return std::nullopt;
    }
    const char *value = request.url_params.get(key.c_str());
    parameters.emplace(key, value ? value : "");
    if (separator == std::string_view::npos)
      break;
    query.remove_prefix(separator + 1);
  }
  return parameters;
}

std::optional<std::optional<std::int64_t>>
parseIntegerParameter(const QueryParameters &parameters,
                      const std::string_view name,
                      const std::int64_t minimum,
                      const std::int64_t maximum) {
  const auto found = parameters.find(std::string(name));
  if (found == parameters.end())
    return std::optional<std::int64_t>{};
  std::int64_t parsed = 0;
  const std::string_view text(found->second);
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (text.empty() || result.ec != std::errc{} ||
      result.ptr != text.data() + text.size() || parsed < minimum ||
      parsed > maximum) {
    return std::nullopt;
  }
  return std::optional<std::int64_t>{parsed};
}

std::optional<std::string_view> idempotencyKey(const crow::request &request) {
  const std::string &key = request.get_header_value("Idempotency-Key");
  if (!core::application::IdempotencyService::isUuid(key))
    return std::nullopt;
  return key;
}

} // namespace ncs::server::controller
