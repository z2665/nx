// res/unique_handle.hpp —— P2 泄漏圈禁（批次 5，roadmap §5.2）：Win32 句柄的唯一 RAII 形态。
// 纪律：CloseHandle/RegCloseKey/FreeLibrary 等释放调用自此只允许出现在 src/res/，
// 其余代码一律经 UniqueFile/UniqueRegKey/UniqueModule 持有句柄
// （tests/audit_ownership.py 的 grep 圈禁检查为硬门）。
#pragma once
#include "gsl_owner.hpp"
#include <windows.h>
#include <winreg.h>
#include <utility>

namespace nx::res {

// T=句柄类型，Invalid=无效哨兵值，Closer=释放函数。move-only；
// out() 供 RegOpenKeyExW 等 Win32 API 的接收端参数（先释放旧值再取地址）。
template <typename T, T Invalid, void (*Closer)(T) noexcept>
class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(T h) noexcept : h_(h) {}
    ~UniqueHandle() {
        if (h_ != Invalid) Closer(h_);
    }
    UniqueHandle(UniqueHandle&& o) noexcept : h_(std::exchange(o.h_, Invalid)) {}
    UniqueHandle& operator=(UniqueHandle&& o) noexcept {
        if (this != &o) {
            if (h_ != Invalid) Closer(h_);
            h_ = std::exchange(o.h_, Invalid);
        }
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    T get() const noexcept { return h_; }
    bool valid() const noexcept { return h_ != Invalid; }
    explicit operator bool() const noexcept { return valid(); }
    // 所有权逃逸点（gsl::owner 标注）：接收方负责最终释放——须立即交给
    // 另一 RAII 持有者或进程级缓存（如 7z.dll 终身持有）
    gsl::owner<T> release() noexcept { return std::exchange(h_, Invalid); }
    void reset(T h = Invalid) noexcept {
        T old = std::exchange(h_, h);
        if (old != Invalid) Closer(old);
    }
    T* out() noexcept {
        reset();
        return &h_;
    }

private:
    T h_ = Invalid;
};

inline void close_handle(HANDLE h) noexcept { CloseHandle(h); }
inline void close_reg_key(HKEY h) noexcept { RegCloseKey(h); }
inline void free_module(HMODULE h) noexcept { FreeLibrary(h); }

using UniqueFile = UniqueHandle<HANDLE, INVALID_HANDLE_VALUE, close_handle>;
using UniqueRegKey = UniqueHandle<HKEY, nullptr, close_reg_key>;
using UniqueModule = UniqueHandle<HMODULE, nullptr, free_module>;

} // namespace nx::res
