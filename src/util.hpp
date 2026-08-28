// nx - 流式嵌套压缩包解压工具（M0）
// util.hpp：通用工具 —— 转换、路径、错误类型
#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <cstdint>

namespace nx {

using byte = unsigned char;

// ---- 错误类型（决定退出码语义，见设计 §8）----
struct Error : std::runtime_error {
    explicit Error(std::string m) : std::runtime_error(std::move(m)) {}
};
struct LimitError : Error {            // 超限熔断 / 磁盘水位 → 退出码 3
    explicit LimitError(std::string m) : Error(std::move(m)) {}
};
struct PasswordExhausted : Error {     // 密码缺失或耗尽 → 退出码 2
    std::string layer;
    PasswordExhausted(std::string layer_, std::string m)
        : Error(std::move(m)), layer(std::move(layer_)) {}
};
struct MissingVolumes : Error {        // 缺分片 → 退出码 4
    explicit MissingVolumes(std::string m) : Error(std::move(m)) {}
};
struct CorruptError : Error {          // 数据损坏（keep-going 可隔离）→ 记入退出码 1
    explicit CorruptError(std::string m) : Error(std::move(m)) {}
};

// ---- 编码转换 ----
std::wstring utf8_to_wide(std::string_view s);
std::string  wide_to_utf8(std::wstring_view s);

// ---- Win32 路径辅助 ----
// 统一 \\?\ 长路径前缀（设计 §4 Windows 集成要点）
std::wstring win_long_path(std::wstring p);
bool ensure_dir_recursive(const std::wstring& dir);   // 递归创建，已存在返回 true
std::wstring win32_last_error_text();
// 临时文件路径：GetTempPath2（可被 tempDir 覆盖）
std::wstring make_temp_file_path(const std::wstring& tempDir);

// ---- 其他 ----
uint64_t parse_size(std::string_view s);              // "512G"/"64MiB"/"1048576"
std::string format_size(uint64_t n);                  // 人类可读

} // namespace nx
