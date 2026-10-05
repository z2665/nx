#include "log.hpp"
#include "sink.hpp"
#include "diag.hpp"
#include "gui.hpp"
#include "res/temp_file.hpp"   // P2 圈禁：UniqueFile + DeleteGuard
#include <bcrypt.h>
#pragma comment(lib, "Bcrypt.lib")
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <atomic>

namespace nx {

namespace {

bool is_reserved_name(const std::string& base) {
    std::string b = ascii_lower(base.substr(0, base.find('.')));
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

std::wstring join_rel(const std::wstring& root, const std::string& rel) {
    std::wstring r = root;
    if (!r.empty() && r.back() != L'\\' && r.back() != L'/') r += L"\\";
    std::wstring w = utf8_to_wide(rel);
    for (auto& c : w)
        if (c == L'/') c = L'\\';   // 统一反斜杠：父目录定位与 \\?\ 前缀兼容
    r += w;
    return r;
}

std::string to_hex(const unsigned char* p, size_t n) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += hex[p[i] >> 4];
        s += hex[p[i] & 15];
    }
    return s;
}

// BCrypt 增量 SHA256（替代 OpenSSL 静态链路：映像更小、启动更快）
struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = false;
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
            BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0)
            ok = true;
    }
    ~Sha256() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void update(const void* p, size_t n) {
        // 失败即失能（finish 返回空）——绝不静默给出错误哈希
        if (ok && BCryptHashData(h, static_cast<PUCHAR>(const_cast<void*>(p)),
                                 static_cast<ULONG>(n), 0) != 0)
            ok = false;
    }
    std::string finish() {
        unsigned char md[32];
        if (!ok || BCryptFinishHash(h, md, sizeof(md), 0) != 0) return {};
        return to_hex(md, sizeof(md));
    }
};

} // namespace

std::string sanitize_segment(const std::string& seg0) {
    std::string s = seg0;
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) out += '_';
        else if (c == ':') out += '_';           // ADS 冒号（D6）
        else if (c == '"' || c == '<' || c == '>' || c == '|' || c == '?' || c == '*') out += '_';
        else if (c == '\\') out += '_';          // 段内反斜杠纵深防御——正常路径
                                                 // 分隔符已被 sanitize_rel 拆分，
                                                 // 残留者按非法字符替换（红队 C1：
                                                 // 混合分隔符曾以此逃逸输出根）
        else out += static_cast<char>(c);
    }
    s = out;
    // 相对路径特例先于尾部裁剪（否则 ".." 会被裁成空串）
    if (s == "." ) s = "_";
    if (s == "..") s = "__";
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    if (s.empty()) s = "_";
    if (is_reserved_name(s)) s = "_" + s;
    if (s.size() > 200) {
        s = s.substr(0, 200);
        // 截断可再生产生尾部点/空格（红队 m3：Windows 落盘时自动剥除会致
        // 落点名与登记名不一致）——同规则复查；全剥空回退占位名
        while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
        if (s.empty()) s = "_";
    }
    return s;
}

Sink::Sink(std::wstring outRoot, const Options& opt, Stats& stats, bool dryRun)
    : outRoot_(std::move(outRoot)), opt_(opt), stats_(stats), dryRun_(dryRun),
      verify_(opt.verify == "sha256") {
    // D4：写出线程池 min(8, cores/2)；dryRun 不需要
    if (!dryRun_)
        pool_ = std::make_unique<ThreadPool>(ThreadPool::defaultWorkers());
}

Sink::~Sink() {
    // S5 INV-SINK：destroyed 仅可自 draining 迁入——有线程池却没 waitAll 过，
    // 异步写还在跑就析构 = 任务悬垂（宏关闭零开销）
    diag::assert_true(!pool_ || waited_, "S5 ~Sink：未 waitAll 即析构（INV-SINK 违反）");
}

void Sink::note(const std::string& line, int depth) {
    // M7：统一走 log_out（尊重 quiet + 进 nx.log）——原直写 printf 绕过日志体系
    log_out("%*s%s\n", depth * 2, "", line.c_str());
}

void Sink::recordHardError(const std::string& msg) {
    stats_.firstHardError.set(msg);   // D3：槽内自带互斥（原 errM_ 只保护写端）
    stats_.abortFlag.store(true);
}

std::string sanitize_rel(const std::string& rel) {
    std::string sanitized;
    size_t start = 0;
    for (;;) {
        // 双分隔符分割：'\' 与 '/' 同为段边界——混合名（a/b\..\..\x）里的
        // 反斜杠穿越段同样进入段级中和（".."→"__"），不得只按 '/' 拆分后
        // 把段内 '\' 原样保留给 join_rel/GetFullPathNameW（C1 zip-slip 根因）
        size_t j = rel.find_first_of("/\\", start);
        std::string seg = (j == std::string::npos) ? rel.substr(start) : rel.substr(start, j - start);
        if (!seg.empty()) {
            if (!sanitized.empty()) sanitized += '/';
            sanitized += sanitize_segment(seg);
        }
        if (j == std::string::npos) break;
        start = j + 1;
    }
    return sanitized.empty() ? "_" : sanitized;
}

std::string Sink::dedupe(const std::string& rel) {
    // 消毒（纯，sanitize_rel；D6：..、绝对路径、保留名、ADS、尾部点/空格、控制字符）
    // + 大小写不敏感重名登记（状态在本对象，消毒与登记分离）
    std::string r = sanitize_rel(rel);
    std::lock_guard<std::mutex> lk(m_);
    std::string low = ascii_lower(r);
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
        if (usedLower_.insert(ascii_lower(cand)).second) return cand;
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

std::string Sink::emitFile(const std::string& rel, std::shared_ptr<ByteSource> src,
                           uint64_t expectedSize, int displayDepth, bool independent) {
    if (stats_.abortFlag.load()) {
        if (gui::progress_cancelled()) throw Cancelled("用户取消");
        std::string he = stats_.firstHardError.get();
        throw Error(he.empty() ? "已中止" : he);
    }
    std::string r = dedupe(rel);
    if (dryRun_) {
        uint64_t sz = expectedSize != UINT64_MAX ? expectedSize : 0;
        note(r + (expectedSize == UINT64_MAX ? " (?)" : " (" + format_size(sz) + ")"),
             displayDepth);
        stats_.filesOut.fetch_add(1);
        return r;
    }
    std::wstring finalPath = join_rel(outRoot_, r);

    if (independent && pool_) {
        // D4：独立源（spool/文件支撑，不受迭代前进影响）→ 线程池异步写。
        // KeepAlive（所有权模型 fixed 变体）：条目源对读取器只持弱引用，
        // 任务期保活令牌在此捕获——任务可超出 walker 栈帧存活，读者不可先亡
        Sink* self = this;
        auto keepAlive = src->keepAlive();
        pool_->submit([self, r, finalPath, expectedSize, displayDepth,
                       src = std::move(src), keepAlive = std::move(keepAlive)]() mutable {
            try {
                self->writeOne(r, finalPath, expectedSize, *src, displayDepth);
            } catch (std::exception& e) {
                self->recordHardError(e.what());
                throw;
            }
        });
        return r;
    }
    writeOne(r, finalPath, expectedSize, *src, displayDepth);
    return r;
}

void Sink::writeOne(const std::string& r, const std::wstring& finalPath, uint64_t expectedSize,
                    ByteSource& src, int displayDepth) {
    gui::progress_file(r);   // 进度窗：当前写出文件（未显示时为廉价 no-op）
    size_t slash = finalPath.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash > 6) {
        if (!ensure_dir_recursive(finalPath.substr(0, slash)))
            throw Error("创建目录失败: " + wide_to_utf8(finalPath));
    }
    // 磁盘水位（D6：每文件写前复查即周期复查）
    if (expectedSize != UINT64_MAX && expectedSize > 0) {
        ULARGE_INTEGER freeBytes{};
        std::wstring dirRoot = finalPath.substr(0, slash == std::wstring::npos ? 3 : slash);
        if (GetDiskFreeSpaceExW(dirRoot.c_str(), &freeBytes, nullptr, nullptr) &&
            freeBytes.QuadPart < static_cast<ULONGLONG>(expectedSize))
            throw LimitError("磁盘空间不足（需要 " + format_size(expectedSize) + "）");
    }

    std::wstring tmp = finalPath + L".nxpart-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64() & 0xFFFFFF);
    // \\?\ 长路径前缀（与 ensure_dir_recursive 同一规范）：裸路径 >260 字符时
    // CreateFileW/MoveFileExW 报的是 ERROR_PATH_NOT_FOUND 而非"路径过长"——
    // 真实案例：251 字符最终路径 + .nxpart 后缀超限，目录全建成、文件全失败
    std::wstring tmpL = win_long_path(tmp);
    std::wstring finalL = win_long_path(finalPath);
    // .part 半成品守卫（P2 圈禁）：落名成功（dismiss）前任何失败路径
    // 自动删除不留盘——原两段 catch 手工清理归一。
    // 声明必须先于句柄：逆序析构 = 先关句柄再删文件（独占句柄未关则删除必败）
    res::DeleteGuard part(tmpL);
    res::UniqueFile h = res::adopt_file(CreateFileW(tmpL.c_str(), GENERIC_WRITE, 0, nullptr,
                                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                                    nullptr));
    if (!h.valid())
        throw Error("创建输出文件失败: " + wide_to_utf8(finalPath) + " " +
                    wide_to_utf8(win32_last_error_text()));

    Sha256 sha;
    bool doHash = verify_;
    uint64_t written = 0;
    std::vector<byte> buf;   // 仅零拷贝不可用时使用
    for (;;) {
        // D5：优先零拷贝视图（libarchive 块 / spool 窗口直借）
        std::span<const byte> v = src.read_direct(1 << 20);
        if (v.empty()) {
            if (buf.empty()) buf.assign(256 << 10, 0);
            size_t n = src.read(buf);
            if (n == 0) break;
            v = std::span<const byte>(buf.data(), n);
        }
        size_t n = v.size();
        size_t off = 0;
        while (off < n) {
            DWORD w = 0;
            if (!WriteFile(h.get(), v.data() + off, static_cast<DWORD>(n - off), &w,
                           nullptr) ||
                w == 0)
                throw Error("写输出失败: " + wide_to_utf8(finalPath) + " " +
                            wide_to_utf8(win32_last_error_text()));
            off += w;
        }
        if (doHash) sha.update(v.data(), n);
        written += n;
        uint64_t total = stats_.bytesOut.fetch_add(n) + n;
        if (total > opt_.maxBytes) {
            stats_.limitTripped = true;
            throw LimitError("累计输出超过上限 " + format_size(opt_.maxBytes));
        }
        // 大文件写出中途响应中止/取消（GUI 取消 → Cancelled → 静默退出）
        if (stats_.abortFlag.load()) {
            if (gui::progress_cancelled())
                throw Cancelled("用户取消");
            throw Error("已中止（写出被全局终止）: " + r);
        }
    }
    h.reset();   // 先关句柄再落名（原语义：CloseHandle 后 MoveFileExW）

    if (expectedSize != UINT64_MAX && written != expectedSize)
        throw CorruptError("条目大小不符: " + r + "（期望 " + std::to_string(expectedSize) +
                           "，实得 " + std::to_string(written) + "）");
    if (!MoveFileExW(tmpL.c_str(), finalL.c_str(), MOVEFILE_REPLACE_EXISTING))
        throw Error("落名失败: " + wide_to_utf8(finalPath) + " " +
                    wide_to_utf8(win32_last_error_text()));
    part.dismiss();   // 落名成功：半成品已是正式文件，不再删除
    stats_.filesOut.fetch_add(1);
    if (doHash) {
        std::string hex = sha.finish();
        std::lock_guard<std::mutex> lk(m_);
        verified_.push_back(VerifiedFile{r, written, std::move(hex)});
    }
    note(r + " (" + format_size(written) + ")", displayDepth);
}

void Sink::waitAll() {
    if (pool_) {
        waited_ = true;   // S5：draining 迁入标记（异常路径同样算已等待）
        try {
            pool_->waitAll();
        } catch (std::exception& e) {
            recordHardError(e.what());
        }
    }
}

} // namespace nx
