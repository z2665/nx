// nx - 流式嵌套压缩包解压工具（M0）
// util.hpp：通用工具 —— 转换、路径、解析。错误分类学与 Result 在 outcome.hpp
//（util.hpp 经 include 转发，既有包含点无需改动）
#pragma once
#include "outcome.hpp"
#include <cstdint>
#include <string>
#include <string_view>

namespace nx {

using byte = unsigned char;

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
// ASCII 小写化（locale 无关）。仅用于扩展名/错误消息/保留名等程序侧判读，
// 不得用于用户可见内容（CJK/非 ASCII 原样保留）
std::string ascii_lower(std::string_view s);

} // namespace nx
