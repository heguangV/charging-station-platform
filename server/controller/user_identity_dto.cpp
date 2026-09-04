#include "server/controller/user_identity_dto.h"

#include "core/application/security_crypto.h"
#include "core/application/user_identity_service.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QImageReader>
#include <QPainter>

#include <cctype>

namespace ncs::server::controller {

std::string maskedPhone(const std::string_view phone)
{
    if (phone.size() != 11) return "***";
    return std::string(phone.substr(0, 3)) + "****" + std::string(phone.substr(7));
}

std::int64_t unixSeconds(const std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

QJsonObject userJson(
    const core::application::UserAccount &user,
    const bool includeAccountState)
{
    QJsonObject result{
        {QStringLiteral("id"), QJsonValue(static_cast<qint64>(user.id))},
        {QStringLiteral("username"), QString::fromStdString(user.username)},
        {QStringLiteral("phoneMasked"), QString::fromStdString(maskedPhone(user.phone))},
        {QStringLiteral("nickname"), QString::fromStdString(user.nickname)},
        {QStringLiteral("avatarUrl"), user.avatar
             ? QJsonValue(QStringLiteral("/api/v1/user/me/avatar/content"))
             : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("status"), user.status},
        {QStringLiteral("statusText"), user.status == 1
             ? QStringLiteral("正常") : QStringLiteral("冻结")},
        {QStringLiteral("registeredAt"), QJsonValue(static_cast<qint64>(user.registeredAt))},
    };
    if (includeAccountState) {
        result.insert(QStringLiteral("balanceCent"), QJsonValue(static_cast<qint64>(user.balanceCent)));
        result.insert(QStringLiteral("debtCent"), QJsonValue(static_cast<qint64>(user.debtCent)));
        result.insert(QStringLiteral("hasActiveFlow"), user.hasActiveFlow);
        result.insert(QStringLiteral("version"), QJsonValue(static_cast<qint64>(user.version)));
    }
    return result;
}

QJsonObject loginJson(const core::application::LoginResult &login)
{
    return {
        {QStringLiteral("accessToken"), QString::fromStdString(login.session.accessToken)},
        {QStringLiteral("expiresAt"), QJsonValue(static_cast<qint64>(
             unixSeconds(login.session.context.expiresAt)))},
        {QStringLiteral("sessionId"), QJsonValue(static_cast<qint64>(
             login.session.context.sessionId))},
        {QStringLiteral("user"), userJson(login.user, false)},
    };
}

namespace {

std::string_view trimWhitespace(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

// Extracts the RFC 2045 boundary parameter wherever it appears in the header
// and tolerates trailing parameters such as "; charset=utf-8" or quoting.
std::optional<std::string> extractBoundary(const std::string_view contentType)
{
    std::string_view remainder = contentType;
    while (!remainder.empty()) {
        const auto separator = remainder.find(';');
        const std::string_view parameter = remainder.substr(
            0, separator == std::string_view::npos ? remainder.size() : separator);
        remainder = separator == std::string_view::npos
            ? std::string_view{} : remainder.substr(separator + 1);
        const auto equals = parameter.find('=');
        if (equals == std::string_view::npos) continue;
        const auto name = trimWhitespace(parameter.substr(0, equals));
        auto value = trimWhitespace(parameter.substr(equals + 1));
        constexpr std::string_view boundaryName = "boundary";
        if (name.size() != boundaryName.size()) continue;
        bool nameMatches = true;
        for (std::size_t index = 0; index < name.size(); ++index) {
            if (std::tolower(static_cast<unsigned char>(name[index]))
                != boundaryName[index]) {
                nameMatches = false;
                break;
            }
        }
        if (!nameMatches) continue;
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        return std::string(value);
    }
    return std::nullopt;
}

} // namespace

std::optional<ParsedAvatar> parseAndNormalizeAvatar(
    const std::string_view contentType,
    const std::string &body)
{
    if (contentType.rfind("multipart/form-data", 0) != 0
        || body.size() > 5 * 1024 * 1024) {
        return std::nullopt;
    }
    const auto boundaryValue = extractBoundary(contentType);
    if (!boundaryValue || boundaryValue->empty()) return std::nullopt;
    const std::string boundary = "--" + *boundaryValue;
    if (boundary.size() < 3 || boundary.size() > 82) return std::nullopt;
    if (boundary.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789'()+_,-./:=?--")
        != std::string::npos) {
        return std::nullopt;
    }
    const auto part = body.find(boundary);
    const auto headerEnd = body.find("\r\n\r\n", part);
    if (part == std::string::npos || headerEnd == std::string::npos
        || headerEnd - part > 8192) return std::nullopt;
    const std::string headers = body.substr(part, headerEnd - part);
    if (headers.find("name=\"file\"") == std::string::npos) return std::nullopt;
    const auto dataStart = headerEnd + 4;
    const auto dataEnd = body.find("\r\n" + boundary, dataStart);
    if (dataEnd == std::string::npos || dataEnd <= dataStart) return std::nullopt;
    const auto closing = dataEnd + 2 + boundary.size();
    if (closing + 2 > body.size() || body.compare(closing, 2, "--") != 0) {
        return std::nullopt;
    }
    const QByteArray input(body.data() + dataStart, static_cast<qsizetype>(dataEnd - dataStart));
    QBuffer source;
    source.setData(input);
    source.open(QIODevice::ReadOnly);
    QImageReader reader(&source);
    const QByteArray format = reader.format().toLower();
    if (format != "png" && format != "jpeg" && format != "jpg" && format != "bmp") {
        return std::nullopt;
    }
    const QSize dimensions = reader.size();
    if (!dimensions.isValid() || dimensions.width() > 4096 || dimensions.height() > 4096) {
        return std::nullopt;
    }
    const QImage image = reader.read();
    if (image.isNull()) return std::nullopt;
    // Avatars are displayed small; downsampling here bounds both the re-encoded
    // PNG size and the memory a single crafted upload can pin per account.
    constexpr int maximumStoredDimension = 1024;
    constexpr auto maximumNormalizedBytes = std::size_t{2} * 1024 * 1024;
    QSize stored = image.size();
    if (stored.width() > maximumStoredDimension || stored.height() > maximumStoredDimension) {
        stored = stored.scaled(
            maximumStoredDimension, maximumStoredDimension, Qt::KeepAspectRatio);
    }
    QImage sanitized(stored, QImage::Format_ARGB32);
    sanitized.fill(Qt::transparent);
    {
        QPainter painter(&sanitized);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(QRect(QPoint(0, 0), stored), image);
    }
    QByteArray normalized;
    QBuffer destination(&normalized);
    destination.open(QIODevice::WriteOnly);
    if (!sanitized.save(&destination, "PNG")
        || static_cast<std::size_t>(normalized.size()) > maximumNormalizedBytes) {
        return std::nullopt;
    }
    core::application::AvatarData avatar;
    avatar.bytes.assign(normalized.begin(), normalized.end());
    avatar.contentType = "image/png";
    avatar.etag = "\"" + core::application::sha256Hex(
        std::string_view(normalized.constData(), normalized.size())) + "\"";
    return ParsedAvatar{std::move(avatar)};
}

} // namespace ncs::server::controller
