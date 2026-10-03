// res/temp_file.hpp —— 唯一临时文件工厂（批次 5 P2 圈禁，roadmap §5.2）。
// TempFile：FILE_FLAG_DELETE_ON_CLOSE——句柄一关（正常析构/异常退出/进程被杀）
// 内核即删，清理责任不再依赖对象生命周期（15GB spool 残留案例的教训）。
// DeleteGuard："临时名 → 成功 rename 终名"模式的半成品守卫——句柄已关、
// 无法再靠 RAII 句柄兜底的删除路径（sink 的 .part 即此形态）。
#pragma once
#include "unique_handle.hpp"
#include "../util.hpp"
#include <string>
#include <utility>

namespace nx::res {

struct TempFile {
    UniqueFile handle;
    std::wstring path;   // 绝对路径（spool 溢出卷/诊断展示用；读经同一句柄，不按路径重开）

    // 在 dir 下创建 nx-{GUID}.tmp（读写 + TEMPORARY | DELETE_ON_CLOSE，CREATE_ALWAYS）。
    // 失败抛 Error("创建临时文件失败: ...")——与原 spool.cpp 文案一致。
    static TempFile create(const std::wstring& dir) {
        std::wstring p = make_temp_file_path(dir);
        UniqueFile h = adopt_file(CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                              nullptr, CREATE_ALWAYS,
                                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                              nullptr));
        if (!h.valid())
            throw Error("创建临时文件失败: " + wide_to_utf8(win32_last_error_text()));
        return TempFile{std::move(h), std::move(p)};
    }
};

// 路径删除守卫：析构时若未 dismiss 则删除。专用于"先写临时名、成功后 rename"的
// 失败清理（.part 半成品不留盘）——成功路径 dismiss() 后不删。
class DeleteGuard {
public:
    explicit DeleteGuard(std::wstring path) : p_(std::move(path)) {}
    ~DeleteGuard() {
        if (!p_.empty()) DeleteFileW(p_.c_str());
    }
    DeleteGuard(const DeleteGuard&) = delete;
    DeleteGuard& operator=(const DeleteGuard&) = delete;
    void dismiss() noexcept { p_.clear(); }

private:
    std::wstring p_;
};

} // namespace nx::res
