// =============================================================================
// MolecuLens · entity · result.h —— Result<T> 错误处理基础设施
// 规格来源：主方案 §14.5 开发约定（解析/IO 用 Result<T>，携带错误码 + 消息）
// 依赖约束：仅 <cstdint>/<string>/<optional>/<utility>，无模块内部类型
// =============================================================================

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace moleculens::entity {

enum class ErrorCode : uint8_t {
    Ok        = 0,
    NotFound,      // 文件/资源不存在
    ParseError,    // 解析失败（格式错误）
    IoError,       // 读写失败
    FormatError,   // 格式不支持
    VersionError,  // 版本不匹配
    CrcError,      // 校验失败
    InvalidState,  // 状态非法
    OutOfMemory,
    Cancelled,
    Unknown,
};

struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string message;
};

// 携带错误码 + 消息的结果类型（主方案 §14.5）
template <typename T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : error_(std::move(error)) {}
    Result(ErrorCode code, std::string message)
        : error_(Error{code, std::move(message)}) {}

    static Result error(ErrorCode code, std::string message) {
        return Result(Error{code, std::move(message)});
    }
    static Result ok(T value) { return Result(std::move(value)); }

    bool has_value() const { return error_ == std::nullopt; }
    explicit operator bool() const { return has_value(); }

    T& value() { return value_.value(); }
    const T& value() const { return value_.value(); }
    T& operator*() { return value_.value(); }
    const T& operator*() const { return value_.value(); }
    T* operator->() { return &value_.value(); }
    const T* operator->() const { return &value_.value(); }

    const Error& error() const { return error_.value(); }
    std::string errorMessage() const {
        return error_ ? error_->message : std::string{};
    }

private:
    std::optional<T> value_;
    std::optional<Error> error_;
};

// void 特化
template <>
class Result<void> {
public:
    Result() : ok_(true) {}
    Result(Error error) : ok_(false), error_(std::move(error)) {}
    Result(ErrorCode code, std::string message)
        : ok_(false), error_(Error{code, std::move(message)}) {}

    static Result<void> error(ErrorCode code, std::string message) {
        return Result<void>(Error{code, std::move(message)});
    }
    static Result<void> ok() { return Result<void>(); }

    bool has_value() const { return ok_; }
    explicit operator bool() const { return ok_; }

    const Error& error() const { return error_.value(); }
    std::string errorMessage() const {
        return error_ ? error_->message : std::string{};
    }

private:
    bool ok_;
    std::optional<Error> error_;
};

} // namespace moleculens::entity
