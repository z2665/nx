#include "sink.hpp"
#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace nx {

namespace {

bool is_reserved_name(const std::string& base) {
    std::string b;
    for (char c : base) {
        if (c == '.') break;
        b += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (b.empty()) return false;
    static const char* reserved[] = {
        "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    };
    for (const char* r : reserved)
        if (b == r) return true;
    return false;
}

// 大小写不敏感的 UTF-8 小写（近似：ASCII 小写化 + 非 ASCII 原样；Windows 碰撞主要来自 ASCII）
std::string lower_ascii(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::wstring join_rel(const std::wstring& root, const std::string& rel) {
    std::wstring r = root;
    if (!r.empty() && r.back() != L'\\' && r.back() != L'/') r += L"\\";
    std::wstring w = utf8_to_wide(rel);
    for (auto& c : w)
        if (c == L'/') c = L'\\';   // 统一反斜杠：父目录定位与 \\?\ 前缀兼容
    r += w;
    return r;
}

} // namespace

std::string sanitize_segment(const std::string& seg0) {
    std::string s = seg0;
    // 控制字符 / 替换非法字符
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) out += '_';
        else if (c == ':') out += '_';           // ADS 冒号（D6）
        else if (c == '"' || c == '<' || c == '>' || c == '|' || c == '?' || c == '*') out += '_';
        else out += static_cast<char>(c);
    }
    s = out;
    // 相对路径特例先于尾部裁剪（否则 ".." 会被裁成空串）
    if (s == "." ) s = "_";
    if (s == "..") s = "__";
    // 尾部点/空格
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    if (s.empty()) s = "_";
    if (is_reserved_name(s)) s = "_" + s;
    // 段长限制
    if (s.size() > 200) s = s.substr(0, 200);
    return s;
}

Sink::Sink(std::wstring outRoot, const Options& opt, Stats& stats, bool dryRun)
    : outRoot_(std::move(outRoot)), opt_(opt), stats_(stats), dryRun_(dryRun) {}

void Sink::note(const std::string& line, int depth) {
    std::printf("%*s%s\n", depth * 2, "", line.c_str());
}

std::string Sink::dedupe(const std::string& rel) {
    // 逐段消毒（D6：..、绝对路径、保留名、ADS、尾部点/空格、控制字符）
    std::string sanitized;
    size_t start = 0;
    for (;;) {
        size_t j = rel.find('/', start);
        std::string seg = (j == std::string::npos) ? rel.substr(start) : rel.substr(start, j - start);
        if (!seg.empty()) {
            if (!sanitized.empty()) sanitized += '/';
            sanitized += sanitize_segment(seg);
        }
        if (j == std::string::npos) break;
        start = j + 1;
    }
    std::string r = sanitized.empty() ? "_" : sanitized;
    std::lock_guard<std::mutex> lk(m_);
    std::string low = lower_ascii(r);
    if (usedLower_.insert(low).second) return r;   // 首用
    // 在最后扩展名前插入 " (n)"
    size_t slash = r.find_last_of('/');
    std::string dir = slash == std::string::npos ? "" : r.substr(0, slash + 1);
    std::string name = slash == std::string::npos ? r : r.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    std::string base = (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
    std::string ext = (dot == std::string::npos || dot == 0) ? "" : name.substr(dot);
    for (int n = 2;; ++n) {
        std::string cand = dir + base + " (" + std::to_string(n) + ")" + ext;
        if (usedLower_.insert(lower_ascii(cand)).second) return cand;
    }
}

void Sink::emitDir(const std::string& rel, int displayDepth) {
    std::string r = dedupe(rel);
    if (dryRun_) {
        note(r + "/", displayDepth);
        return;
    }
    if (!ensure_dir_recursive(join_rel(outRoot_, r)))
        throw Error("创建目录失败: " + r);
    note(r + "/", displayDepth);
}

std::string Sink::emitFile(const std::string& rel, ByteSource& src, uint64_t expectedSize,
                           int displayDepth) {
    if (stats_.abortFlag.load()) throw Error(stats_.firstHardError);
    std::string r = dedupe(rel);
    if (dryRun_) {
        uint64_t sz = expectedSize != UINT64_MAX ? expectedSize : 0;
        note(r + (expectedSize == UINT64_MAX ? " (?)" : " (" + format_size(sz) + ")"),
             displayDepth);
        stats_.filesOut.fetch_add(1);
        return r;
    }

    std::wstring finalPath = join_rel(outRoot_, r);
    // 目录
    size_t slash = finalPath.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash > 6) {
        if (!ensure_dir_recursive(finalPath.substr(0, slash)))
            throw Error("创建目录失败: " + wide_to_utf8(finalPath));
    }
    // 磁盘水位预检（D6：expectedSize 已知时）
    if (expectedSize != UINT64_MAX && expectedSize > 0) {
        ULARGE_INTEGER freeBytes{};
        std::wstring dirRoot = finalPath.substr(0, slash == std::wstring::npos ? 3 : slash);
        if (GetDiskFreeSpaceExW(dirRoot.c_str(), &freeBytes, nullptr, nullptr) &&
            freeBytes.QuadPart < static_cast<ULONGLONG>(expectedSize))
            throw LimitError("磁盘空间不足（需要 " + format_size(expectedSize) + "）");
    }

    std::wstring tmp = finalPath + L".nxpart-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(stats_.filesOut.load());
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw Error("创建输出文件失败: " + wide_to_utf8(finalPath) + " " +
                    wide_to_utf8(win32_last_error_text()));

    uint64_t written = 0;
    try {
        std::vector<byte> buf(256 << 10);
        for (;;) {
            size_t n = src.read(buf);
            if (n == 0) break;
            size_t off = 0;
            while (off < n) {
                DWORD w = 0;
                if (!WriteFile(h, buf.data() + off, static_cast<DWORD>(n - off), &w, nullptr) ||
                    w == 0)
                    throw Error("写输出失败: " + wide_to_utf8(finalPath) + " " +
                                wide_to_utf8(win32_last_error_text()));
                off += w;
            }
            written += n;
            uint64_t total = stats_.bytesOut.fetch_add(n) + n;
            if (total > opt_.maxBytes) {
                stats_.limitTripped = true;
                throw LimitError("累计输出超过上限 " + format_size(opt_.maxBytes));
            }
        }
    } catch (...) {
        CloseHandle(h);
        DeleteFileW(tmp.c_str());
        throw;
    }
    CloseHandle(h);

    try {
        if (expectedSize != UINT64_MAX && written != expectedSize)
            throw CorruptError("条目大小不符: " + r + "（期望 " + std::to_string(expectedSize) +
                               "，实得 " + std::to_string(written) + "）");
        if (!MoveFileExW(tmp.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING))
            throw Error("落名失败: " + wide_to_utf8(finalPath) + " " +
                        wide_to_utf8(win32_last_error_text()));
    } catch (...) {
        DeleteFileW(tmp.c_str());   // 校验/落名失败不留 .part
        throw;
    }
    stats_.filesOut.fetch_add(1);
    note(r + " (" + format_size(written) + ")", displayDepth);
    return r;
}

} // namespace nx
