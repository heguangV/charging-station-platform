#include "message_frame.h"
#include <limits>
namespace ncs::device_sim
{
namespace
{
bool id(const QJsonValue& v, qint64& out)
{
    if (!v.isDouble())
        return false;
    const auto n = v.toInteger(-1);
    if (n < 1)
        return false;
    out = n;
    return true;
}
bool object(const QJsonValue& v, QJsonObject& out)
{
    if (!v.isObject())
        return false;
    out = v.toObject();
    return true;
}
bool fail(QString* e, const QString& text)
{
    if (e)
        *e = text;
    return false;
}
} // namespace
QJsonArray MessageFrame::encodeCall(qint64 id, const QString& action, const QJsonObject& payload)
{
    return {2, id, action, payload};
}
QJsonArray MessageFrame::encodeResult(qint64 id, const QJsonObject& payload)
{
    return {3, id, payload};
}
QJsonArray MessageFrame::encodeError(qint64 id, const QString& code, const QString& description,
                                     const QJsonObject& details)
{
    return {4, id, code, description, details};
}
bool MessageFrame::decode(const QJsonArray& raw, Frame& f, QString* error)
{
    if (raw.size() < 3 || raw.size() > 5)
        return fail(error, "frame length is invalid");
    qint64 messageId = 0;
    if (!id(raw.at(1), messageId))
        return fail(error, "message id is invalid");
    f = {};
    f.messageId = messageId;
    const int type = raw.at(0).toInt(-1);
    if (type == 2)
    {
        if (raw.size() != 4 || !raw.at(2).isString() || raw.at(2).toString().isEmpty() ||
            !object(raw.at(3), f.payload))
            return fail(error, "call frame is invalid");
        f.kind = Frame::Kind::Call;
        f.action = raw.at(2).toString();
        return true;
    }
    if (type == 3)
    {
        if (raw.size() != 3 || !object(raw.at(2), f.payload))
            return fail(error, "result frame is invalid");
        f.kind = Frame::Kind::Result;
        return true;
    }
    if (type == 4)
    {
        if (raw.size() != 5 || !raw.at(2).isString() || !raw.at(3).isString() ||
            !object(raw.at(4), f.details))
            return fail(error, "error frame is invalid");
        f.kind = Frame::Kind::Error;
        f.errorCode = raw.at(2).toString();
        f.errorDescription = raw.at(3).toString();
        return true;
    }
    return fail(error, "frame type is unknown");
}
} // namespace ncs::device_sim
