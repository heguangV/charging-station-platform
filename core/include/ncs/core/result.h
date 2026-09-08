// Qt 侧通用 Result<T>：以 variant 持有“成功值或 AppError
// 失败”，用于客户端本地服务/网络层的返回值传递。 与 ncs/core/error.h 配套；经静态
// success()/failure() 构造，用 hasValue()/operator bool 判定后取值。 Result<void>
// 特化表达无载荷操作；失败时取 value() 会触发 std::get 抛异常，须先判定。

#pragma once

#include "ncs/core/error.h"

#include <utility>
#include <variant>

namespace ncs::core
{

template <typename T> class Result final
{
  public:
    static Result success(T value)
    {
        return Result(std::move(value));
    }
    static Result failure(AppError error)
    {
        return Result(std::move(error));
    }

    bool hasValue() const noexcept
    {
        return std::holds_alternative<T>(value_);
    }
    explicit operator bool() const noexcept
    {
        return hasValue();
    }
    const T& value() const
    {
        return std::get<T>(value_);
    }
    T& value()
    {
        return std::get<T>(value_);
    }
    const AppError& error() const
    {
        return std::get<AppError>(value_);
    }

  private:
    explicit Result(T value) : value_(std::move(value)) {}
    explicit Result(AppError error) : value_(std::move(error)) {}

    std::variant<T, AppError> value_;
};

template <> class Result<void> final
{
  public:
    static Result success()
    {
        return Result(true, {});
    }
    static Result failure(AppError error)
    {
        return Result(false, std::move(error));
    }

    bool hasValue() const noexcept
    {
        return succeeded_;
    }
    explicit operator bool() const noexcept
    {
        return hasValue();
    }
    const AppError& error() const
    {
        return error_;
    }

  private:
    Result(bool succeeded, AppError error) : succeeded_(succeeded), error_(std::move(error)) {}

    bool succeeded_ = false;
    AppError error_;
};

} // namespace ncs::core
