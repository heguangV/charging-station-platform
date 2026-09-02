#include "ncs/core/error.h"

namespace ncs::core
{

QString errorCodeName(const ErrorCode code)
{
    switch (code)
    {
    case ErrorCode::Ok:
        return QStringLiteral("OK");
    case ErrorCode::InvalidArgument:
        return QStringLiteral("INVALID_ARGUMENT");
    case ErrorCode::ValidationFailed:
        return QStringLiteral("VALIDATION_FAILED");
    case ErrorCode::DatabaseError:
        return QStringLiteral("DATABASE_ERROR");
    case ErrorCode::NotFound:
        return QStringLiteral("NOT_FOUND");
    case ErrorCode::AlreadyExists:
        return QStringLiteral("ALREADY_EXISTS");
    case ErrorCode::UserFrozen:
        return QStringLiteral("USER_FROZEN");
    case ErrorCode::InsufficientBalance:
        return QStringLiteral("INSUFFICIENT_BALANCE");
    case ErrorCode::ChargerUnavailable:
        return QStringLiteral("CHARGER_UNAVAILABLE");
    case ErrorCode::ActiveFlowExists:
        return QStringLiteral("ACTIVE_FLOW_EXISTS");
    case ErrorCode::AllocationConflict:
        return QStringLiteral("ALLOCATION_CONFLICT");
    case ErrorCode::TransactionFailed:
        return QStringLiteral("TRANSACTION_FAILED");
    case ErrorCode::ExternalServiceUnavailable:
        return QStringLiteral("EXTERNAL_SERVICE_UNAVAILABLE");
    case ErrorCode::InternalError:
        return QStringLiteral("INTERNAL_ERROR");
    case ErrorCode::IdempotencyConflict:
        return QStringLiteral("IDEMPOTENCY_CONFLICT");
    case ErrorCode::InvalidStateTransition:
        return QStringLiteral("INVALID_STATE_TRANSITION");
    case ErrorCode::QuoteExpired:
        return QStringLiteral("QUOTE_EXPIRED");
    case ErrorCode::ReservationExpired:
        return QStringLiteral("RESERVATION_EXPIRED");
    case ErrorCode::DebtOutstanding:
        return QStringLiteral("DEBT_OUTSTANDING");
    case ErrorCode::RateLimited:
        return QStringLiteral("RATE_LIMITED");
    case ErrorCode::CodeInvalid:
        return QStringLiteral("CODE_INVALID");
    case ErrorCode::CodeExpired:
        return QStringLiteral("CODE_EXPIRED");
    case ErrorCode::VersionConflict:
        return QStringLiteral("VERSION_CONFLICT");
    case ErrorCode::ReauthRequired:
        return QStringLiteral("REAUTH_REQUIRED");
    case ErrorCode::Unauthorized:
        return QStringLiteral("UNAUTHORIZED");
    case ErrorCode::Forbidden:
        return QStringLiteral("FORBIDDEN");
    }
    return QStringLiteral("INTERNAL_ERROR");
}

} // namespace ncs::core
