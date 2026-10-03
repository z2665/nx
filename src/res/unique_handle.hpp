// res/unique_handle.hpp —— P2 泄漏圈禁（批次 5，roadmap §5.2）：Win32 句柄的唯一 RAII 形态。
// 纪律：释放调用只允许出现在 src/res/——名单以 tests/audit_ownership.py 的
// RELEASE_CALL 为准（CloseHandle/DeleteFileW/RegCloseKey/FreeLibrary/->Release()），
// 其余代码一律经 UniqueFile/UniqueRegKey/UniqueModule 持有句柄（audit 硬门）。
#pragma once
#include "gsl_owner.hpp"
#include <windows.h>
#include <winreg.h>
#include <utility>

namespace nx::res {

// T=句柄类型，Invalid=无效哨兵值，Closer=释放函数。move-only；
// out() 供 RegOpenKeyExW 等 Win32 API 的接收端参数（先释放旧值再取地址）。
// 哨兵须为合法 NTTP 常量：三别名统一 nullptr（各 API 失败值即 NULL）。唯一例外
// 是 CreateFileW 族——失败返回 INVALID_HANDLE_VALUE，而该宏含整数→指针 cast，
// 不是常量表达式、clang/cl 全家拒绝其为模板实参（评审 C-1：曾使 AST 审计门
// 在错误恢复模式下非确定运行）——文件句柄一律经 adopt_file 归一后接入。
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

using UniqueFile = UniqueHandle<HANDLE, nullptr, close_handle>;
using UniqueRegKey = UniqueHandle<HKEY, nullptr, close_reg_key>;
using UniqueModule = UniqueHandle<HMODULE, nullptr, free_module>;

// CreateFileW 族返回值的接入点（评审 C-1）：失败值 INVALID_HANDLE_VALUE 归一为
// 空哨兵。接收 owner 标注（传入即所有权转移）；直造 UniqueFile(h) 仅限已验有效的句柄
inline UniqueFile adopt_file(gsl::owner<HANDLE> h) noexcept {
    return UniqueFile(h == INVALID_HANDLE_VALUE ? nullptr : h);
}

} // namespace nx::res
