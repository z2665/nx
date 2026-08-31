#include "util.hpp"
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <objbase.h>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Shlwapi.lib")

namespace nx {

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(s.data()),
                                static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(s.data()),
                        static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring win_long_path(std::wstring p) {
    if (p.compare(0, 4, L"\\\\?\\") == 0) return p;
    if (p.compare(0, 8, L"\\\\?\\UNC\\") == 0) return p;
    // 相对路径必须先转绝对（\\?\ 不支持相对）
    wchar_t abs[MAX_PATH * 4];
    if (PathIsRelativeW(p.c_str())) {
        wchar_t cwd[MAX_PATH * 4];
        GetCurrentDirectoryW(MAX_PATH * 4, cwd);
        std::wstring full = std::wstring(cwd) + L"\\" + p;
        if (GetFullPathNameW(full.c_str(), MAX_PATH * 4, abs, nullptr) == 0) return p;
        p = abs;
    } else {
        if (GetFullPathNameW(p.c_str(), MAX_PATH * 4, abs, nullptr) == 0) return p;
        p = abs;
    }
    if (p.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

bool ensure_dir_recursive(const std::wstring& dir) {
    if (dir.empty()) return false;
    std::wstring p = win_long_path(dir);
    if (CreateDirectoryW(p.c_str(), nullptr)) return true;
    DWORD e = GetLastError();
    // ALREADY_EXISTS 可能是"目录已存在"也可能是"同名文件占位"（真实案例：
    // extract-into 默认前缀曾=完整文件名 → 输出目录与输入 zip 同名，本级误报
    // 成功、子目录创建才失败）——必须验证现存路径确为目录
    if (e == ERROR_ALREADY_EXISTS)
        return (GetFileAttributesW(p.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (e == ERROR_PATH_NOT_FOUND) {
        size_t pos = p.find_last_of(L"\\/");
        if (pos == std::wstring::npos || pos <= 4 /*\\?\c:*/) return false;
        if (!ensure_dir_recursive(p.substr(0, pos))) return false;
        if (CreateDirectoryW(p.c_str(), nullptr) != 0) return true;
        return GetLastError() == ERROR_ALREADY_EXISTS &&
               (GetFileAttributesW(p.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return false;
}

std::wstring win32_last_error_text() {
    DWORD e = GetLastError();
    wchar_t buf[512];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, e, 0, buf, 512, nullptr);
    std::string s = wide_to_utf8(std::wstring(buf, n));
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    char code[32];
    std::snprintf(code, sizeof(code), " (Win32 %lu)", static_cast<unsigned long>(e));
    return utf8_to_wide(s + code);
}

std::wstring make_temp_file_path(const std::wstring& tempDir) {
    wchar_t base[MAX_PATH * 2];
    if (!tempDir.empty()) {
        wcsncpy_s(base, tempDir.c_str(), _TRUNCATE);
    } else {
        // GetTempPath2：按进程隔离临时目录（Win10 1607+）
        if (!GetTempPath2W(MAX_PATH, base)) GetTempPathW(MAX_PATH, base);
    }
    GUID g;
    if (CoCreateGuid(&g) != S_OK) {
        std::swprintf(base, MAX_PATH, L"%u-%lu.tmp", GetCurrentProcessId(), GetTickCount());
        return utf8_to_wide("nx-") + base;
    }
    wchar_t guid[64];
    StringFromGUID2(g, guid, 64) ;
    std::wstring dir(base);
    if (!dir.empty() && dir.back() != L'\\') dir += L"\\";
    return dir + L"nx-" + guid + L".tmp";
}

uint64_t parse_size(std::string_view s) {
    if (s.empty()) throw Error("空的大小字符串");
    uint64_t base = 0; size_t i = 0;
    bool neg = s[0] == '-';
    if (neg) throw Error("大小不能为负数: " + std::string(s));
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i)
        base = base * 10 + static_cast<uint64_t>(s[i] - '0');
    uint64_t mult = 1;
    if (i < s.size()) {
        char c = static_cast<char>(::tolower(s[i]));
        bool binary = (i + 1 < s.size() && (s[i + 1] == 'i' || s[i + 1] == 'I')) ||
                      i + 1 == s.size() - (s.size() - i - 1 == 1 ? 0 : 1); // K/M/G/T[IB][B]
        (void)binary; // 统一按二进制解释（设计文档语义）
        switch (c) {
            case 'k': mult = 1ull << 10; break;
            case 'm': mult = 1ull << 20; break;
            case 'g': mult = 1ull << 30; break;
            case 't': mult = 1ull << 40; break;
            case 'b': mult = 1; break;         // "100B"
            default: throw Error("无法识别的大小单位: " + std::string(s));
        }
    }
    return base * mult;
}

std::string format_size(uint64_t n) {
    char buf[64];
    if (n >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.2f GiB", n / 1073741824.0);
    else if (n >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.2f MiB", n / 1048576.0);
    else if (n >= (1ull << 10)) std::snprintf(buf, sizeof(buf), "%.1f KiB", n / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(n));
    return buf;
}

} // namespace nx
