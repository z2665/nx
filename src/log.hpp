// log.hpp：默认日志（M3 需求 6）—— nx.exe 所在目录 nx.log
// 始终 append；超过 5 MiB 截断从 0 开始；控制台与文件双写。
#pragma once
#include <cstdarg>
#include <string>

namespace nx {

// 进程启动时调用一次：打开（或截断）日志，写运行头
void log_open(int argc, char** utf8ArgsDummy);
// 常规输出（stdout + 文件）与错误输出（stderr + 文件）
void log_out(const char* fmt, ...);
void log_err(const char* fmt, ...);
// 任意文本块（如 report JSON）直接入文件
void log_raw(const std::string& text);
// 日志文件绝对路径（诊断）
std::wstring log_path();

} // namespace nx
