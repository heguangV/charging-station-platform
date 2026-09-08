// 应用服务统一返回值模板：ErrorCode + 可选载荷 T，ok() 表示成功且载荷存在。
// core/application 各服务的返回约定；错误码取值于 core/domain/error_code.h，由控制器层映射为 HTTP
// 状态与错误响应。 失败时无载荷，调用方须先判断 ok() 再取 value()。

#pragma once

#include "core/domain/error_code.h"

#include <optional>

namespace ncs::core::application
{

template <typename T> struct ServiceResult
{
    core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
    std::optional<T> value;
    bool ok() const
    {
        return error == core::domain::ErrorCode::Ok && value.has_value();
    }
};

} // namespace ncs::core::application
