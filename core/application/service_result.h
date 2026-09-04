#pragma once

#include "core/domain/error_code.h"

#include <optional>

namespace ncs::core::application {

template<typename T>
struct ServiceResult {
    core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
    std::optional<T> value;
    bool ok() const { return error == core::domain::ErrorCode::Ok && value.has_value(); }
};

} // namespace ncs::core::application
