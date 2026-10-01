#include "log.hpp"
#include "util.hpp"
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace nx {

namespace {

constexpr uint64_t kMaxLogBytes = 5ull * 1024 * 1024;   // 5 MiB：超过截断从 0 开始

std::mutex g_logMx;
FILE* g_logFile = nullptr;
std::wstring g_logPath;
// fuzz 静音/GUI/写出线程并发读写（roadmap D5）——普通 bool 是数据竞争 UB
std::atomic<bool> g_quiet{false};

void close_log() {
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
}

} // namespace

std::wstring log_path() {
    return g_logPath;
}

void log_open(int argc, char** utf8ArgsDummy) {
    (void)argc;
    (void)utf8ArgsDummy;
    std::lock_guard<std::mutex> lk(g_logMx);
    if (g_logFile) return;
    // nx.exe 所在目录\nx.log；不可写则退回系统临时目录
    wchar_t exe[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
    if (n == 0 || n >= MAX_PATH * 4) return;
    std::wstring dir(exe);
    size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos) dir.resize(slash);
    g_logPath = dir + L"\\nx.log";

    HANDLE probe = CreateFileW(g_logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        g_logPath = std::wstring(tmp) + L"nx.log";
        probe = CreateFileW(g_logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe == INVALID_HANDLE_VALUE) return;
    }
    // 超过 5 MiB → 截断从 0 开始
    LARGE_INTEGER sz{};
    GetFileSizeEx(probe, &sz);
    if (static_cast<uint64_t>(sz.QuadPart) > kMaxLogBytes)
        SetFilePointerEx(probe, {}, nullptr, FILE_BEGIN), SetEndOfFile(probe);
    else
        SetFilePointerEx(probe, {}, nullptr, FILE_END);
    CloseHandle(probe);
    _wfopen_s(&g_logFile, g_logPath.c_str(), L"ab");   // 之后始终 append
    if (!g_logFile) return;

    // 运行头：时间戳 + 完整命令行（密码红线过滤，D1：-p/--password/--password-file
    // 的值以 *** 替换——密码绝不入日志是项目第一安全纪律）
    SYSTEMTIME st;
    GetLocalTime(&st);
    int nArgs = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    char head[256];
    std::snprintf(head, sizeof(head), "==== nx %04u-%02u-%02u %02u:%02u:%02u pid=%lu ====",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  GetCurrentProcessId());
    std::string line = head;
    if (argv) {
        auto isPwFlag = [](const std::string& a) {
            return a == "-p" || a == "--password" || a == "--password-file";
        };
        bool redactNext = false;
        for (int i = 0; i < nArgs; ++i) {
            line += " ";
            if (redactNext) {   // 上一记是密码开关：本记即密码值 → 脱敏
                line += "***";
                redactNext = false;
            } else {
                std::string tok = wide_to_utf8(argv[i]);
                redactNext = isPwFlag(tok);
                line += tok;
            }
        }
        LocalFree(argv);
    }
    line += "\n";
    std::fwrite(line.data(), 1, line.size(), g_logFile);
    std::fflush(g_logFile);
}

static void vwrite(bool err, const char* fmt, va_list ap) {
    char buf[4096];
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n <= 0) return;
    if (n >= static_cast<int>(sizeof(buf))) n = sizeof(buf) - 1;
    // 控制台（无控制台时写无效句柄，无害；quiet 时跳过——fuzz 高频调用防刷屏/拖速）
    if (!g_quiet)
        std::fwrite(buf, 1, static_cast<size_t>(n), err ? stderr : stdout);
    std::lock_guard<std::mutex> lk(g_logMx);
    if (g_logFile) {
        std::fwrite(buf, 1, static_cast<size_t>(n), g_logFile);
        std::fflush(g_logFile);
    }
}

void log_set_quiet(bool v) {
    g_quiet = v;
}

bool log_console_enabled() {
    return !g_quiet;
}

void log_out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwrite(false, fmt, ap);
    va_end(ap);
}

void log_err(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwrite(true, fmt, ap);
    va_end(ap);
}

void log_raw(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (g_logFile) {
        std::fwrite(text.data(), 1, text.size(), g_logFile);
        std::fflush(g_logFile);
    }
}

} // namespace nx
