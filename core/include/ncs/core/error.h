// Qt 侧共享错误模型：ErrorCode 枚举与 AppError 结构（诊断信息、用户文案、请求 ID）。
// 枚举值与 core/domain/error_code.h 的公开错误码保持同一取值表（只增不改），供 Qt 客户端解释 REST
// 错误响应。 使用 QString 承载展示与日志文本；errorCodeName() 返回稳定字符串码。

#pragma once

#include <QString>

namespace ncs::core
{

enum class ErrorCode : int
{
    Ok = 0,
    InvalidArgument = 1,
    ValidationFailed = 2,
    DatabaseError = 3,
    NotFound = 4,
    AlreadyExists = 5,
    UserFrozen = 6,
    InsufficientBalance = 7,
    ChargerUnavailable = 8,
    ActiveFlowExists = 9,
    AllocationConflict = 10,
    TransactionFailed = 11,
    ExternalServiceUnavailable = 12,
    InternalError = 13,
    IdempotencyConflict = 14,
    InvalidStateTransition = 15,
    QuoteExpired = 16,
    ReservationExpired = 17,
    DebtOutstanding = 18,
    RateLimited = 19,
    CodeInvalid = 20,
    CodeExpired = 21,
    VersionConflict = 22,
    ReauthRequired = 23,
    Unauthorized = 401,
    Forbidden = 403,
};

struct AppError final
{
    ErrorCode code = ErrorCode::InternalError;
    QString diagnostic;
    QString userMessage;
    QString requestId;
};

QString errorCodeName(ErrorCode code);

} // namespace ncs::core
