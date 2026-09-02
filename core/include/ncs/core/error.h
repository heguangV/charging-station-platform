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
