#pragma once
// 统一错误封装：全项目只用 Result<T>，不混用 optional
#include <string>
#include <utility>

namespace axr {

template <typename T>
struct Result {
    T value{};
    std::string err;

    bool ok() const { return err.empty(); }

    static Result Ok(T v) {
        Result r;
        r.value = std::move(v);
        return r;
    }
    static Result Fail(std::string e) {
        Result r;
        r.err = std::move(e);
        return r;
    }
};

// 无返回值的版本
struct Status {
    std::string err;
    bool ok() const { return err.empty(); }
    static Status Ok() { return {}; }
    static Status Fail(std::string e) { return Status{std::move(e)}; }
};

} // namespace axr
