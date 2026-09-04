#pragma once

#include <string_view>

namespace ncs::core::domain {

enum class ErrorCode : int {
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

constexpr int httpStatus(const ErrorCode code)
{
    switch (code) {
    case ErrorCode::Ok: return 200;
    case ErrorCode::InvalidArgument: return 400;
    case ErrorCode::ValidationFailed:
    case ErrorCode::CodeInvalid:
    case ErrorCode::CodeExpired: return 422;
    case ErrorCode::DatabaseError:
    case ErrorCode::TransactionFailed:
    case ErrorCode::ExternalServiceUnavailable: return 503;
    case ErrorCode::NotFound: return 404;
    case ErrorCode::AlreadyExists:
    case ErrorCode::InsufficientBalance:
    case ErrorCode::ChargerUnavailable:
    case ErrorCode::ActiveFlowExists:
    case ErrorCode::AllocationConflict:
    case ErrorCode::IdempotencyConflict:
    case ErrorCode::InvalidStateTransition:
    case ErrorCode::QuoteExpired:
    case ErrorCode::ReservationExpired:
    case ErrorCode::DebtOutstanding:
    case ErrorCode::VersionConflict: return 409;
    case ErrorCode::UserFrozen:
    case ErrorCode::Forbidden: return 403;
    case ErrorCode::InternalError: return 500;
    case ErrorCode::RateLimited: return 429;
    case ErrorCode::ReauthRequired:
    case ErrorCode::Unauthorized: return 401;
    }
    return 500;
}

constexpr std::string_view errorCodeName(const ErrorCode code)
{
    switch (code) {
    case ErrorCode::Ok: return "OK";
    case ErrorCode::InvalidArgument: return "INVALID_ARGUMENT";
    case ErrorCode::ValidationFailed: return "VALIDATION_FAILED";
    case ErrorCode::DatabaseError: return "DATABASE_ERROR";
    case ErrorCode::NotFound: return "NOT_FOUND";
    case ErrorCode::AlreadyExists: return "ALREADY_EXISTS";
    case ErrorCode::UserFrozen: return "USER_FROZEN";
    case ErrorCode::InsufficientBalance: return "INSUFFICIENT_BALANCE";
    case ErrorCode::ChargerUnavailable: return "CHARGER_UNAVAILABLE";
    case ErrorCode::ActiveFlowExists: return "ACTIVE_FLOW_EXISTS";
    case ErrorCode::AllocationConflict: return "ALLOCATION_CONFLICT";
    case ErrorCode::TransactionFailed: return "TRANSACTION_FAILED";
    case ErrorCode::ExternalServiceUnavailable: return "EXTERNAL_SERVICE_UNAVAILABLE";
    case ErrorCode::InternalError: return "INTERNAL_ERROR";
    case ErrorCode::IdempotencyConflict: return "IDEMPOTENCY_CONFLICT";
    case ErrorCode::InvalidStateTransition: return "INVALID_STATE_TRANSITION";
    case ErrorCode::QuoteExpired: return "QUOTE_EXPIRED";
    case ErrorCode::ReservationExpired: return "RESERVATION_EXPIRED";
    case ErrorCode::DebtOutstanding: return "DEBT_OUTSTANDING";
    case ErrorCode::RateLimited: return "RATE_LIMITED";
    case ErrorCode::CodeInvalid: return "CODE_INVALID";
    case ErrorCode::CodeExpired: return "CODE_EXPIRED";
    case ErrorCode::VersionConflict: return "VERSION_CONFLICT";
    case ErrorCode::ReauthRequired: return "REAUTH_REQUIRED";
    case ErrorCode::Unauthorized: return "UNAUTHORIZED";
    case ErrorCode::Forbidden: return "FORBIDDEN";
    }
    return "INTERNAL_ERROR";
}

} // namespace ncs::core::domain
