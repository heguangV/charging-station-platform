#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace ncs::device_sim
{
struct Frame
{
    enum class Kind
    {
        Call,
        Result,
        Error
    } kind = Kind::Call;
    qint64 messageId = 0;
    QString action;
    QJsonObject payload;
    QString errorCode;
    QString errorDescription;
    QJsonObject details;
};

class MessageFrame final
{
  public:
    static QJsonArray encodeCall(qint64 id, const QString& action, const QJsonObject& payload);
    static QJsonArray encodeResult(qint64 id, const QJsonObject& payload);
    static QJsonArray encodeError(qint64 id, const QString& code, const QString& description,
                                  const QJsonObject& details = {});
    static bool decode(const QJsonArray& raw, Frame& frame, QString* error = nullptr);
};
} // namespace ncs::device_sim
