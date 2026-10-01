#include "engines.hpp"
#include "diag.hpp"
#include "log.hpp"
// ContainerReader/EngineOptions 契约见 container.hpp
#include <windows.h>
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <cctype>
#include <deque>
#include <cstring>
#include <mutex>
#include <vector>

namespace nx {

namespace {

// ---- 错误分类（§6.2：必须区分"密码错"与"数据坏"）----
enum class FailKind { Password, Corrupt, Other };

// ---- 文件名编码修复（设计 §3.2：EFS 位 UTF-8 vs CP437）----
// 无 EFS 标志的 zip/tar 条目名是本地码表（CP932/GBK…）原始字节；libarchive 按
// CP437 兜底转成"合法但乱码"的 UTF-8（或漏转直接给原始字节），经 utf8_to_wide
// 失败后消毒成 "_"。此处：反推 CP437 恢复原始字节 → 严格 UTF-8 → CJK 码表评分
// 选择（假名加分=日文，系统 ACP 平手优先=中文），每进程粘性（同一包语言一致）。
int g_nameCp = 0;

bool strict_decode(const std::string& raw, int cp, std::wstring* out) {
    int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.data(),
                                static_cast<int>(raw.size()), nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()),
                        w.data(), n);
    if (out) *out = std::move(w);
    return true;
}

int score_w(const std::wstring& w) {
    int kana = 0, cjk = 0, halfKana = 0, weird = 0;
    for (wchar_t c : w) {
        if (c >= 0x3040 && c <= 0x30FF)
            kana++;   // 全角平/片假名：日文强特征
        else if (c >= 0xFF66 && c <= 0xFF9D)
            halfKana++;   // 半角片假名：GBK 流被误按 932 解的典型征兆
        else if (c >= 0x4E00 && c <= 0x9FFF)
            cjk++;
        else if (c < 0x20)
            weird++;
    }
    return kana * 4 + cjk - halfKana * 2 - weird * 8;
}

std::string fix_archive_name(const char* nm) {
    if (!nm) return "";
    std::string s(nm);
    bool high = false;
    for (unsigned char c : s)
        if (c >= 0x80) { high = true; break; }
    if (!high) return s;

    // 原始字节候选：s 本身非法 UTF-8（libarchive 漏转）或反推 CP437（乱码转换可逆）
    std::string raw = s;
    std::wstring cur;
    if (strict_decode(s, CP_UTF8, &cur)) {
        // s 是合法 UTF-8：真 EFS 名（含 CP437 外字符 → 保留）或 CP437 乱码（可逆）
        char buf[4096];
        BOOL usedDef = FALSE;
        int n = WideCharToMultiByte(437, 0, cur.data(), static_cast<int>(cur.size()), buf,
                                    sizeof(buf), nullptr, &usedDef);
        if (usedDef || n <= 0)
            return s;   // 含 CP437 外字符 → 真实 Unicode 名
        // 长名截断保护（4096 内不适用）
        raw.assign(buf, static_cast<size_t>(n));
        // 反推后再试严格 UTF-8：无 EFS 但实为 UTF-8 的写入器
        std::wstring retry;
        if (strict_decode(raw, CP_UTF8, &retry)) {
            bool retryHigh = false;
            for (wchar_t c : retry)
                if (c >= 0x80) { retryHigh = true; break; }
            if (retryHigh)
                return wide_to_utf8(retry);
            return s;   // 反推后全 ASCII：原名即 ASCII 安全
        }
    }

    // CJK 码表选择：粘性优先，否则评分（候选顺序 = 平手优先级）
    int acp = GetACP();
    int cand[5] = {acp, 936, 950, 949, 932};
    if (g_nameCp) {
        std::wstring w;
        if (strict_decode(raw, g_nameCp, &w))
            return wide_to_utf8(w);
    }
    int bestCp = 0, bestScore = INT_MIN;
    std::wstring bestW;
    for (int cp : cand) {
        std::wstring w;
        if (!strict_decode(raw, cp, &w)) continue;
        int sc = score_w(w);
        if (sc > bestScore) {
            bestScore = sc;
            bestCp = cp;
            bestW = std::move(w);
        }
    }
    if (bestCp) {
        g_nameCp = bestCp;
        return wide_to_utf8(bestW);
    }
    return s;   // 全部失败：保留 libarchive 原名（不再产生空名）
}

FailKind classify_msg(const char* m) {
    if (!m) return FailKind::Other;
    std::string s = ascii_lower(m);
    if (s.find("passphrase") != std::string::npos || s.find("password") != std::string::npos)
        return FailKind::Password;
    if (s.find("crc") != std::string::npos || s.find("damaged") != std::string::npos ||
        s.find("truncat") != std::string::npos || s.find("invalid") != std::string::npos ||
        s.find("unrecognized") != std::string::npos || s.find("unsupported") != std::string::npos ||
        s.find("bad") != std::string::npos || s.find("corrupt") != std::string::npos)
        return FailKind::Corrupt;
    return FailKind::Other;
}

// ---- 随机访问视图（zip 中央目录模式：spool 或根文件，M3 文件名修复）----
class SeekView : public RegionSource {
public:
    virtual ~SeekView() = default;
    virtual size_t read_at(uint64_t pos, std::span<byte> buf) = 0;
    virtual uint64_t size() const = 0;
};

class SpoolSeekView : public SeekView {
public:
    explicit SpoolSeekView(std::shared_ptr<SpoolBuffer> s) : s_(std::move(s)) {}
    size_t read_at(uint64_t pos, std::span<byte> buf) override { return s_->read_at(pos, buf); }
    uint64_t size() const override { return s_->size(); }
private:
    std::shared_ptr<SpoolBuffer> s_;
};

class FileSeekView : public SeekView {
public:
    // meter：根输入计量（进度窗分子；null = 码表探测等不计量的临时视图）
    // base/length：窗口（隐写 zip——EOCD 精确区间，排除尾部伪装数据）；length=0 = 到 EOF
    explicit FileSeekView(const std::wstring& path, InputMeter* meter = nullptr,
                          uint64_t base = 0, uint64_t length = 0)
        : meter_(meter), base_(base) {
        h_ = CreateFileW(win_long_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h_ == INVALID_HANDLE_VALUE)
            throw Error("打开文件失败: " + wide_to_utf8(path));
        LARGE_INTEGER sz{};
        GetFileSizeEx(h_, &sz);
        uint64_t total = static_cast<uint64_t>(sz.QuadPart);
        if (base > total || base + (length ? length : (total - base)) > total)
            throw Error("视图窗口越界: " + wide_to_utf8(path));
        size_ = length ? length : (total - base);
    }
    ~FileSeekView() override {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    FileSeekView(const FileSeekView&) = delete;
    FileSeekView& operator=(const FileSeekView&) = delete;
    size_t read_at(uint64_t pos, std::span<byte> buf) override {
        std::lock_guard<std::mutex> lk(m_);
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(base_ + pos);
        if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) throw Error("定位失败");
        size_t got = 0;
        while (got < buf.size()) {
            DWORD r = 0;
            if (!ReadFile(h_, buf.data() + got, static_cast<DWORD>(buf.size() - got), &r,
                          nullptr) || r == 0)
                break;
            got += r;
        }
        if (meter_) meter_->bytes += got;   // 根消耗（重读会被 99% 封顶吸收）
        return got;
    }
    uint64_t size() const override { return size_; }
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
    InputMeter* meter_ = nullptr;
    uint64_t base_ = 0;
    std::mutex m_;
};

// 区间窗口：父 RegionSource（文件视图/spool/另一区间）中 [base, base+len) 的只读视图。
// 嵌套容器免 spool 直读的核心——子引擎把它当作本地小文件随机访问（可链式套窗口）。
class RegionView : public SeekView {
public:
    RegionView(std::shared_ptr<RegionSource> parent, uint64_t base, uint64_t len)
        : parent_(std::move(parent)), base_(base), len_(len) {}
    size_t read_at(uint64_t pos, std::span<byte> buf) override {
        if (pos >= len_) return 0;
        uint64_t avail = len_ - pos;
        size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), avail));
        return parent_->read_at(base_ + pos, std::span<byte>(buf.data(), n));
    }
    uint64_t size() const override { return len_; }
private:
    std::shared_ptr<RegionSource> parent_;
    uint64_t base_, len_;
};

struct CbCtx {
    ByteSource* src = nullptr;      // 流式 = PushbackSource
    SeekView* view = nullptr;       // seekable 模式（spool/文件）
    uint64_t viewPos = 0;           // seekable 模式当前位置
    std::vector<byte> buf;
    // 条目数据相位的视图访问记录（免 spool 直读：推导演区间用）。
    // recActive 在条目首次数据读时置位；recMin/RecMax 记录该相位视图读覆盖范围
    //（stored 条目 = 本地头+载荷的连续读；seek 不计入，读越界无害——只用起点）
    bool recActive = false;
    uint64_t recMin = UINT64_MAX;
    uint64_t recMax = 0;
};

la_ssize_t la_read_cb(archive*, void* c, const void** buf) {
    auto* ctx = static_cast<CbCtx*>(c);
    try {
        size_t n;
        if (ctx->view) {
            uint64_t posBefore = ctx->viewPos;
            n = ctx->view->read_at(ctx->viewPos, ctx->buf);
            ctx->viewPos += n;
            if (ctx->recActive && n) {   // 数据相位访问记录（区间推导）
                if (posBefore < ctx->recMin) ctx->recMin = posBefore;
                if (ctx->viewPos > ctx->recMax) ctx->recMax = ctx->viewPos;
            }
        } else {
            n = ctx->src->read(ctx->buf);
        }
        *buf = ctx->buf.data();
        return static_cast<la_ssize_t>(n);
    } catch (...) {
        return -1;
    }
}

la_int64_t la_seek_cb(archive*, void* c, la_int64_t off, int whence) {
    auto* ctx = static_cast<CbCtx*>(c);
    if (!ctx->view) return -1;
    try {
        uint64_t size = ctx->view->size();
        uint64_t pos = ctx->viewPos;
        uint64_t abs = 0;
        switch (whence) {
            case SEEK_SET: abs = static_cast<uint64_t>(off); break;
            case SEEK_CUR: abs = pos + static_cast<uint64_t>(off); break;
            case SEEK_END: abs = size + static_cast<uint64_t>(off); break;
            default: return -1;
        }
        // 区间推导记录：数据相位的视图读位置（载荷锚点，本地头由回溯扫描定位）
        if (ctx->recActive) {
            if (abs < ctx->recMin) ctx->recMin = abs;
            if (abs > ctx->recMax) ctx->recMax = abs;
        }
        ctx->viewPos = abs;
        return static_cast<la_int64_t>(abs);
    } catch (...) {
        return -1;
    }
}

// zip 文件名码表探测（§3.2）：无 EFS 标志 + 无 hdrcharset 时 libarchive 对
// 无法按 UTF-8 校验的名字返回 NULL → 下游消毒成 "_"。对候选码表逐一试开
// （seekable 模式遍历中央目录，不读数据），按"零空名 + 假名加分"择优。
template <class ViewFactory>
std::string detect_zip_charset(ViewFactory makeView) {
    static int n = 0;
    int acp = GetACP();
    std::vector<std::string> cands;
    cands.push_back("");                       // EFS/纯 ASCII：无需选项
    if (acp != 932 && acp != 936 && acp != 949 && acp != 950 && acp != 1252)
        cands.push_back("CP" + std::to_string(acp));  // 系统码表（非 CJK 默认集时）
    cands.push_back("CP936");                  // GBK
    cands.push_back("CP932");                  // Shift-JIS
    cands.push_back("CP950");                  // Big5
    cands.push_back("CP949");                  // EUC-KR

    std::string best;
    long bestScore = LONG_MIN;
    for (auto& cp : cands) {
        archive* a = archive_read_new();
        archive_read_support_format_zip(a);
        if (!cp.empty()) {
            std::string opt = "zip:hdrcharset=" + cp;
            archive_read_set_options(a, opt.c_str());
        }
        auto view = makeView();
        CbCtx ctx;
        ctx.view = view.get();
        ctx.buf.assign(256 << 10, 0);
        archive_read_set_read_callback(a, la_read_cb);
        archive_read_set_close_callback(a, [](archive*, void*) { return ARCHIVE_OK; });
        archive_read_set_seek_callback(a, la_seek_cb);
        archive_read_set_callback_data(a, &ctx);
        bool ok = archive_read_open1(a) == ARCHIVE_OK;
        long score = 0;
        int highSeen = 0;
        archive_entry* e = nullptr;
        int nullNames = 0;
        while (ok && archive_read_next_header(a, &e) == ARCHIVE_OK) {
            const char* nm = archive_entry_pathname(e);
            if (!nm) {
                ++nullNames;
                break;   // 该码表下仍有空名 → 拒绝
            }
            bool hi = false;
            for (const char* p = nm; *p; ++p)
                if ((unsigned char)*p >= 0x80) { hi = true; break; }
            if (hi) {
                std::wstring w = utf8_to_wide(nm);
                score += score_w(w);
                if (++highSeen >= 128) break;
            }
        }
        archive_read_free(a);
        if (nullNames > 0) continue;
        if (score > bestScore) {
            bestScore = score;
            best = cp;
        }
    }
    return best;
}

// ---- LaSeqReader：libarchive 顺序条目读取器 ----

class LaSeqReader : public ContainerReader, public std::enable_shared_from_this<LaSeqReader> {
public:
    // 三种打开形态：borrowed（流式借用探测）/ view（seekable：spool 或根文件）
    // 流式模式成功后由 caller 调 adoptStream() 过继所有权
    LaSeqReader(archive* a, archive_entry* e,
                PushbackSource* borrowed,
                std::shared_ptr<SpoolBuffer> spool,
                std::shared_ptr<SeekView> view = nullptr)
        : a_(a), e_(e, &archive_entry_free), borrowed_(borrowed),
          spool_(std::move(spool)), view_(std::move(view)) {
        if (view_) {
            ctx_.view = view_.get();
            ctx_.viewPos = 0;
            ctx_.src = nullptr;
        } else {
            ctx_.src = borrowed_;
        }
        (void)spool_;
        ctx_.buf.assign(256 << 10, 0);
    }

    ~LaSeqReader() override {
        if (a_) archive_read_free(a_);
    }

    // 流式模式成功确认后过继所有权（失败路径 caller 保留 src 以便 rewind/spool 回退）
    // 采纳后该流不再需要回看（probe/D2 回退已完成）→ 关历史，启用 D5 直通
    void adoptStream(std::unique_ptr<PushbackSource> s) {
        borrowed_ = nullptr;
        streamingSrc_ = std::move(s);
        ctx_.src = streamingSrc_.get();
        streamingSrc_->setHistoryEnabled(false);
    }

    CbCtx& ctx() { return ctx_; }
    archive* arch() { return a_; }

    // 免 spool 直读：当前条目若为父视图（文件/spool）中的连续 stored 载荷，
    // 返回其精确区间视图。前提：条目数据已经开始经本读取器读取（detect peek /
    // probe 预读皆可）——数据相位首个视图读覆盖本地头，由此解析载荷起点。
    // 解析失败（非 stored/加密/头不合法/未进入数据相位）返回 null → 调用方回退 spool。
    std::shared_ptr<RegionSource> regionOf(int idx) {
        if (!view_ || idx != curIdx_ || !dataPhase_ || sizes_.empty()) return nullptr;
        uint64_t esz = sizes_[std::min<size_t>(idx, sizes_.size() - 1)];
        if (esz == UINT64_MAX || !ctx_.recActive || ctx_.recMin == UINT64_MAX) {
            return nullptr;
        }
        // recMin = 数据相位首个视图读位置（载荷中段或本地头）。
        // 向前回溯定位本地头：PK\x03\x04 + method==0 + 未加密 + 覆盖 recMin
        // 且长度精确 = esz。zip 条目区间互不重叠 → 覆盖 recMin 的 stored 载荷至多
        // 一个；误配由调用方的"子打开失败回退 spool"兜底。
        // 覆盖 libarchive 首块缓冲（256KB）+ 本地头/扩展字段上限（30+64K+64K）
        const uint64_t back = 512 * 1024;
        uint64_t from = ctx_.recMin > back ? ctx_.recMin - back : 0;
        size_t spanLen = static_cast<size_t>(ctx_.recMin - from) + 30;
        std::vector<byte> scan(spanLen);
        if (view_->read_at(from, std::span<byte>(scan)) != spanLen) return nullptr;
        for (size_t p = 0; p + 30 <= scan.size(); ++p) {
            if (scan[p] != 'P' || scan[p + 1] != 'K' || scan[p + 2] != 0x03 ||
                scan[p + 3] != 0x04)
                continue;
            unsigned flags = scan[p + 6] | (scan[p + 7] << 8);
            if (flags & 0x1) continue;   // 加密（12B 密码头在载荷前）→ 回退
            unsigned method = scan[p + 8] | (scan[p + 9] << 8);
            if (method != 0) continue;   // 仅 stored
            unsigned nlen = scan[p + 26] | (scan[p + 27] << 8);
            unsigned elen = scan[p + 28] | (scan[p + 29] << 8);
            uint64_t payload = from + p + 30 + nlen + elen;
            if (payload > ctx_.recMin) continue;              // 载荷须始于首读前
            if (ctx_.recMin >= payload + esz) continue;       // 首读须落在载荷内
            if (payload + esz > view_->size()) continue;
            return std::make_shared<RegionView>(view_, payload, esz);
        }
        return nullptr;
    }

    bool next(ContainerEntry& out) override;

private:
    bool nextInternal(ContainerEntry& out);
public:

    size_t readEntryData(int idx, std::span<byte> buf);
    std::span<const byte> readEntryDirect(int idx, size_t maxN);
    std::optional<uint64_t> entrySize(int idx) const {
        if (idx < 0 || idx >= static_cast<int>(sizes_.size())) return {};
        if (sizes_[idx] == UINT64_MAX) return {};
        return sizes_[idx];
    }

    // 探测：前进到首个含数据的条目并读首块（验证密码）。失败经异常（密码/损坏）。
    // 途经的目录/空文件条目元数据进 replayQ_ 由 next() 重放（顺序保持）；
    // 停在的条目预读字节存 probeFront_ 重放给下游（否则条目流短 1 字节）。
    // 7z 等工具会把目录条目放在最前——只看首条目会漏掉密码验证。
    void probeFirst() {
        for (;;) {
            ContainerEntry tmp;
            if (!nextInternal(tmp)) return;   // 空容器：open 本身即验证
            ReplayRecord rec{curIdx_, std::move(tmp.name), tmp.size, tmp.isDir, tmp.isSymlink,
                             false, std::move(tmp.symlinkTarget)};
            if (tmp.isSymlink || tmp.isDir) {
                replayQ_.push_back(std::move(rec));
                continue;
            }
            if (tmp.size == 0) {   // 空文件：无数据可验，重放时换空流
                rec.nullSrc = true;
                replayQ_.push_back(std::move(rec));
                continue;
            }
            byte b[1];
            size_t got = readEntryData(curIdx_, std::span<byte>(b, 1));
            probeFront_.assign(b, b + got);
            replayQ_.push_back(std::move(rec));
            return;
        }
    }

private:
    friend class LaEntrySource;
    archive* a_;
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)> e_;
    PushbackSource* borrowed_ = nullptr;      // 探测阶段借用
    std::unique_ptr<PushbackSource> streamingSrc_;   // 过继后所有
    std::shared_ptr<SpoolBuffer> spool_;
    std::shared_ptr<SeekView> view_;          // seekable 模式（spool/文件）
    CbCtx ctx_;
    // probe 预取条目重放队列（D6 结构修复）：只存元数据 + 迭代序号，绝不持条目源——
    // LaEntrySource 经 shared_from_this() 持回指读取器的强引用，存进读取器自己的
    // 队列即构成自引用环（15GB 临时文件残留案例根因）：打开失败或中途弃置的读取器
    // 永不析构，连带 spool 与视图泄漏。重放在 next() 现场按 idx 重建源
    struct ReplayRecord {
        int idx;
        std::string name;
        uint64_t size;
        bool isDir, isSymlink, nullSrc;   // nullSrc：size==0 空文件，重放换 NullSource
        std::string symlinkTarget;
    };
    std::deque<ReplayRecord> replayQ_;
    std::vector<byte> probeFront_;         // probe 预读待重放字节
    int curIdx_ = -1;
    std::span<const byte> laBlock_;   // 当前 libarchive 块视图（有效至下一次 data_block 调用）
    size_t blockOff_ = 0;
    uint64_t entryPos_ = 0;      // 已拉入 leftover 的条目内偏移
    std::vector<uint64_t> sizes_;
    bool dataPhase_ = false;     // 当前条目已开始数据读取（激活视图访问记录）
};

class LaEntrySource : public ByteSource {
public:
    LaEntrySource(std::shared_ptr<LaSeqReader> r, int idx) : r_(std::move(r)), idx_(idx) {}
    size_t read(std::span<byte> buf) override { return r_->readEntryData(idx_, buf); }
    // D5：libarchive 块视图直借，省一次 memcpy
    std::span<const byte> read_direct(size_t maxN) override {
        return r_->readEntryDirect(idx_, maxN);
    }
    std::optional<uint64_t> sizeHint() const override { return r_->entrySize(idx_); }
    std::shared_ptr<RegionSource> seekRegion() const override { return r_->regionOf(idx_); }
private:
    std::shared_ptr<LaSeqReader> r_;
    int idx_;
};

bool LaSeqReader::next(ContainerEntry& out) {
    if (!replayQ_.empty()) {
        ReplayRecord rec = std::move(replayQ_.front());
        replayQ_.pop_front();
        out.name = std::move(rec.name);
        out.size = rec.size;
        out.isDir = rec.isDir;
        out.isSymlink = rec.isSymlink;
        out.symlinkTarget = std::move(rec.symlinkTarget);
        out.independentData = false;
        // 重放时重建条目源：目录/空文件本就无数据可读；含数据的停留条目
        // idx == curIdx_（readEntryData 的失效校验依赖它），与 probe 前行为等价
        out.data = rec.nullSrc
                       ? std::shared_ptr<ByteSource>(std::make_shared<NullSource>())
                       : std::make_shared<LaEntrySource>(shared_from_this(), rec.idx);
        return true;
    }
    return nextInternal(out);
}

bool LaSeqReader::nextInternal(ContainerEntry& out) {
    int r = archive_read_next_header2(a_, e_.get());
    if (r == ARCHIVE_EOF) return false;
    if (r != ARCHIVE_OK && r != ARCHIVE_WARN) {
        FailKind fk = classify_msg(archive_error_string(a_));
        std::string m = archive_error_string(a_) ? archive_error_string(a_) : "读取头失败";
        if (fk == FailKind::Password) throw PasswordExhausted("", "加密层密码错误: " + m);
        throw CorruptError("读取归档头失败: " + m);
    }
    ++curIdx_;
    laBlock_ = {};
    blockOff_ = 0;
    entryPos_ = 0;
    dataPhase_ = false;
    ctx_.recActive = false;
    probeFront_.clear();
    const char* nm = archive_entry_pathname(e_.get());
    if (!nm) {
        // 防御（§3.2）：码表探测后仍可能出现空名（未知编码）——合成可辨识名而非 "_"
        log_err("[nx] ! 条目 %d 名字无法解码（未知码表），已合成占位名\n", curIdx_);
        out.name = "__noname_" + std::to_string(curIdx_);
    } else {
        out.name = fix_archive_name(nm);   // §3.2 文件名编码：无 EFS 标志的 CP437 乱码修复
    }
    auto ft = archive_entry_filetype(e_.get());   // la_mode_t（MSVC 无 mode_t）
    out.isDir = (ft & AE_IFMT) == AE_IFDIR;
    out.isSymlink = (ft & AE_IFMT) == AE_IFLNK;
    out.symlinkTarget.clear();
    if (out.isSymlink && archive_entry_symlink(e_.get()))
        out.symlinkTarget = archive_entry_symlink(e_.get());
    out.size = archive_entry_size_is_set(e_.get())
                   ? static_cast<uint64_t>(archive_entry_size(e_.get()))
                   : UINT64_MAX;
    sizes_.push_back(out.size);
    out.data = std::make_shared<LaEntrySource>(shared_from_this(), curIdx_);
    return true;
}

// 零拷贝：优先当前块残留视图；否则拉新块返回其视图（全程无 memcpy）。
// probeFront_ 非空时退回空视图（重放字节须先经 read() 交付）。
std::span<const byte> LaSeqReader::readEntryDirect(int idx, size_t maxN) {
    if (idx != curIdx_ || maxN == 0) return {};
    if (!probeFront_.empty()) return {};
    if (!dataPhase_) {   // 首次数据访问：激活视图访问记录（区间推导前提）
        dataPhase_ = true;
        if (view_) {
            ctx_.recActive = true;
            ctx_.recMin = UINT64_MAX;
            ctx_.recMax = 0;
        }
    }
    if (blockOff_ >= laBlock_.size()) {
        const void* p = nullptr;
        size_t sz = 0;
        la_int64_t off = 0;
        int r = archive_read_data_block(a_, &p, &sz, &off);
        if (r == ARCHIVE_EOF) return {};
        const char* emsg = archive_error_string(a_);
        if (r == ARCHIVE_WARN) {
            std::string low = ascii_lower(emsg ? emsg : "");
            if (low.find("crc") != std::string::npos)
                throw CorruptError(std::string("条目数据损坏(CRC): ") + (emsg ? emsg : ""));
        } else if (r != ARCHIVE_OK) {
            FailKind fk = classify_msg(emsg);
            if (fk == FailKind::Password)
                throw PasswordExhausted("", std::string("条目密码错误: ") + (emsg ? emsg : ""));
            throw CorruptError(std::string("条目数据损坏: ") + (emsg ? emsg : ""));
        }
        if (off != static_cast<la_int64_t>(entryPos_))
            throw CorruptError("条目数据偏移不连续");
        laBlock_ = std::span<const byte>(static_cast<const byte*>(p), sz);
        blockOff_ = 0;
        entryPos_ += sz;
    }
    if (blockOff_ >= laBlock_.size()) return {};
    size_t n = std::min(maxN, laBlock_.size() - blockOff_);
    auto v = laBlock_.subspan(blockOff_, n);
    blockOff_ += n;
    return v;
}

size_t LaSeqReader::readEntryData(int idx, std::span<byte> buf) {
    if (idx != curIdx_) throw Error("条目流已失效（迭代已前进）");
    if (buf.empty()) return 0;
    if (!dataPhase_ && probeFront_.empty()) {   // 首次数据访问：激活视图访问记录
        dataPhase_ = true;
        if (view_) {
            ctx_.recActive = true;
            ctx_.recMin = UINT64_MAX;
            ctx_.recMax = 0;
        }
    }
    if (!probeFront_.empty()) {   // probe 预读字节优先交付
        size_t n = std::min(buf.size(), probeFront_.size());
        std::memcpy(buf.data(), probeFront_.data(), n);
        probeFront_.erase(probeFront_.begin(), probeFront_.begin() + n);
        return n;
    }
    if (blockOff_ < laBlock_.size()) {
        size_t n = std::min(buf.size(), laBlock_.size() - blockOff_);
        std::memcpy(buf.data(), laBlock_.data() + blockOff_, n);
        blockOff_ += n;
        return n;
    }
    const void* p = nullptr;
    size_t sz = 0;
    la_int64_t off = 0;
    int r = archive_read_data_block(a_, &p, &sz, &off);
    if (r == ARCHIVE_EOF) return 0;
    const char* emsg = archive_error_string(a_);
    if (r == ARCHIVE_WARN) {
        std::string low = ascii_lower(emsg ? emsg : "");
        if (low.find("crc") != std::string::npos)
            throw CorruptError(std::string("条目数据损坏(CRC): ") + (emsg ? emsg : ""));
    } else if (r != ARCHIVE_OK) {
        FailKind fk = classify_msg(emsg);
        if (fk == FailKind::Password)
            throw PasswordExhausted("", std::string("条目密码错误: ") + (emsg ? emsg : ""));
        throw CorruptError(std::string("条目数据损坏: ") + (emsg ? emsg : ""));
    }
    if (off != static_cast<la_int64_t>(entryPos_))
        throw CorruptError("条目数据偏移不连续");
    laBlock_ = std::span<const byte>(static_cast<const byte*>(p), sz);
    blockOff_ = 0;
    entryPos_ += sz;
    size_t n = std::min(buf.size(), laBlock_.size());
    std::memcpy(buf.data(), laBlock_.data(), n);
    blockOff_ = n;
    return n;
}

// ---- 构造 helper ----

archive* make_arch(Format fmt) {
    archive* a = archive_read_new();
    if (!a) throw Error("libarchive 分配失败");
    bool ok = true;
    switch (fmt) {
        case Format::Tar: ok = archive_read_support_format_tar(a) == ARCHIVE_OK; break;
        case Format::Cpio: ok = archive_read_support_format_cpio(a) == ARCHIVE_OK; break;
        case Format::Ar: ok = archive_read_support_format_ar(a) == ARCHIVE_OK; break;
        case Format::Zip: ok = archive_read_support_format_zip(a) == ARCHIVE_OK; break;
        case Format::SevenZip: ok = archive_read_support_format_7zip(a) == ARCHIVE_OK; break;
        case Format::Rar:
            ok = archive_read_support_format_rar(a) == ARCHIVE_OK &&
                 archive_read_support_format_rar5(a) == ARCHIVE_OK;
            break;
        case Format::Iso: ok = archive_read_support_format_iso9660(a) == ARCHIVE_OK; break;
        case Format::Cab: ok = archive_read_support_format_cab(a) == ARCHIVE_OK; break;
        default: ok = false;
    }
    if (!ok) {
        archive_read_free(a);
        throw Error("libarchive 不支持该格式: " + std::string(format_name(fmt)));
    }
    return a;
}

struct OpenOutcome {
    std::shared_ptr<LaSeqReader> reader;   // 成功时非空
    FailKind fail = FailKind::Other;
    std::string failMsg;
};

// 打开 + probe（首条目首块验证密码）。不抛异常，失败以 fail/failMsg 表达。
// 流式模式：仅借用 streamingSrc（成功后 caller 调 adoptStream）；失败时所有权不受影响。
OpenOutcome try_open(Format fmt,
                     std::unique_ptr<PushbackSource>& streamingSrc,
                     const std::shared_ptr<SpoolBuffer>& spool,
                     const SecureStr* pw,
                     const std::shared_ptr<SeekView>& view = nullptr,
                     const char* zipCharset = nullptr) {
    OpenOutcome oc;
    diag::TryOpenGuard tog;   // S3：失败出口的读取器必须当场析构（D6 环的案发现场检查）
    archive* a = nullptr;
    try {
        a = make_arch(fmt);
        if (pw && !pw->empty()) archive_read_add_passphrase(a, pw->c_str());
        if (zipCharset && *zipCharset) {
            std::string opt = "zip:hdrcharset=";
            opt += zipCharset;
            archive_read_set_options(a, opt.c_str());
        }
        std::shared_ptr<SeekView> v = view;
        if (!v && spool) v = std::make_shared<SpoolSeekView>(spool);
        auto r = std::make_shared<LaSeqReader>(a, archive_entry_new(),
                                               (!spool && !v) ? streamingSrc.get() : nullptr,
                                               spool, v);
        archive_read_set_read_callback(a, la_read_cb);
        archive_read_set_close_callback(a, [](archive*, void*) { return ARCHIVE_OK; });
        if (r->ctx().view) archive_read_set_seek_callback(a, la_seek_cb);
        archive_read_set_callback_data(a, &r->ctx());
        int res = archive_read_open1(a);
        if (res != ARCHIVE_OK) {
            const char* m = archive_error_string(a);
            oc.fail = classify_msg(m);
            oc.failMsg = m ? m : "open 失败";
            return oc;   // r 析构 → 释放 a；streamingSrc 归 caller
        }
        try {
            r->probeFirst();
        } catch (PasswordExhausted& e) {
            oc.fail = FailKind::Password;
            oc.failMsg = e.what();
            return oc;
        } catch (CorruptError& e) {
            oc.fail = FailKind::Corrupt;
            oc.failMsg = e.what();
            return oc;
        } catch (Error& e) {
            oc.fail = FailKind::Other;
            oc.failMsg = e.what();
            return oc;
        }
        if (!spool && !v) r->adoptStream(std::move(streamingSrc));   // 成功：过继
        oc.reader = std::move(r);
        tog.escaped = true;   // 成功路径 reader 存活合法
        return oc;
    } catch (std::exception& e) {
        if (a) archive_read_free(a);   // r 未建立所有权时
        oc.fail = FailKind::Other;
        oc.failMsg = e.what();
        return oc;
    }
}

std::shared_ptr<SpoolBuffer> spool_all(PushbackSource& src, const EngineOptions& opt) {
    auto s = std::make_shared<SpoolBuffer>(opt.spoolRam, opt.tempDir);
    std::vector<byte> buf(256 << 10);
    for (;;) {
        size_t n = src.read(buf);
        if (n == 0) break;
        s->append(std::span<const byte>(buf.data(), n));
    }
    s->finish();
    return s;
}

// 密码候选迭代循环（§6.2 顺序：缓存→上次成功→候选→交互）
// 返回成功 reader；耗尽抛 PasswordExhausted；损坏抛 CorruptError
std::shared_ptr<LaSeqReader> password_loop(Format fmt,
                                           const std::shared_ptr<SpoolBuffer>& spool,
                                           const std::string& layerId,
                                           PasswordProvider& pw,
                                           const char* zipCharset = nullptr) {
    for (;;) {
        auto cand = pw.nextAttempt(layerId);
        if (!cand)
            throw PasswordExhausted(layerId, "密码缺失或已耗尽: " + layerId);
        std::unique_ptr<PushbackSource> nullSrc{};
        auto oc = try_open(fmt, nullSrc, spool, &*cand, nullptr, zipCharset);
        if (oc.reader) {
            pw.reportSuccess(layerId, *cand);
            return std::move(oc.reader);
        }
        if (oc.fail != FailKind::Password)
            throw CorruptError(oc.failMsg);   // 非"密码错"= 数据坏，直接报损坏
    }
}

} // namespace

// 原生多卷入口（RAR）：直接走 7z.dll（§3.3 原生卷型不拼接）
std::shared_ptr<ContainerReader> open_container_volumes(
    Format fmt, const std::map<std::wstring, sz::VolumeSource>& volumes,
    const std::wstring& firstVol, const std::string& layerId, PasswordProvider& pw,
    const EngineOptions& opt) {
    if (!sz::dll_available())
        throw Error(wide_to_utf8(sz::dll_error()) + "（原生多卷需要 7z.dll）");
    return sz::open_archive(fmt, volumes, firstVol, layerId, pw, opt);
}

// Zip 根文件直读（中央目录模式 + 码表探测；文件本身可 seek，免 spool）。
// base/length：隐写窗口（EOCD 精确区间，排除尾部伪装数据）；默认整文件。
std::shared_ptr<ContainerReader> open_zip_file(const std::wstring& path,
                                               const std::string& layerId,
                                               PasswordProvider& pw,
                                               const EngineOptions& opt,
                                               uint64_t base, uint64_t length) {
    // 码表探测视图不挂 meter：多候选各重读一遍中央目录，会虚增根消耗计数
    std::string cs = detect_zip_charset(
        [&] { return std::make_shared<FileSeekView>(path, nullptr, base, length); });
    auto view = std::make_shared<FileSeekView>(path, opt.meter, base, length);
    std::unique_ptr<PushbackSource> nullSrc{};
    auto oc = try_open(Format::Zip, nullSrc, nullptr, nullptr, view, cs.c_str());
    if (oc.reader) return std::move(oc.reader);
    if (oc.fail == FailKind::Password) {
        // 密码迭代需要重开：在文件视图上直接重试（无需 spool）
        for (;;) {
            auto cand = pw.nextAttempt(layerId);
            if (!cand)
                throw PasswordExhausted(layerId, "密码缺失或已耗尽: " + layerId);
            auto v2 = std::make_shared<FileSeekView>(path, opt.meter, base, length);
            auto oc2 = try_open(Format::Zip, nullSrc, nullptr, &*cand, v2, cs.c_str());
            if (oc2.reader) {
                pw.reportSuccess(layerId, *cand);
                return std::move(oc2.reader);
            }
            if (oc2.fail != FailKind::Password)
                throw CorruptError(oc2.failMsg);
        }
    }
    throw CorruptError(oc.failMsg);
}

std::shared_ptr<ContainerReader> open_container(std::unique_ptr<PushbackSource> src,
                                                Format fmt,
                                                const std::string& layerId,
                                                PasswordProvider& pw,
                                                const EngineOptions& opt,
                                                const std::shared_ptr<RegionSource>& region) {
    // ---- 免 spool 窗口直读：父视图中的连续 stored 区间即子归档完整字节 ----
    // 仅当调用方携带区间（父为 seekable 视图支撑 + stored 条目）时生效；
    // 视图无流语义，密码重试直接重开同一区间；任何打开失败回退下方 spool 原路径
    //（流未被消费——detect 只 peek，PushbackSource 可从 0 重读）。
    auto regionAsView = std::dynamic_pointer_cast<SeekView>(region);

    // ---- Zip：中央目录模式（seekable）+ 码表探测（§3.2 文件名修复）----
    // 不走本地头流式：无 EFS 标志的本地码表名（CP932/GBK…）在流式下会得到
    // NULL pathname（实测 D:\...\2.zip 案例），中央目录 + hdrcharset 才可靠。
    if (fmt == Format::Zip) {
        if (regionAsView) {
            try {
                std::string cs = detect_zip_charset([&] { return regionAsView; });
                std::unique_ptr<PushbackSource> nullSrc{};
                // 首次无密码直开；密码错则按解析链迭代（视图无流语义，重开即可）
                auto oc = try_open(fmt, nullSrc, nullptr, nullptr, regionAsView,
                                   cs.c_str());
                while (!oc.reader && oc.fail == FailKind::Password) {
                    auto cand = pw.nextAttempt(layerId);
                    if (!cand)
                        throw PasswordExhausted(layerId,
                                                "密码缺失或已耗尽: " + layerId);
                    oc = try_open(fmt, nullSrc, nullptr, &*cand, regionAsView,
                                  cs.c_str());
                    if (oc.reader) pw.reportSuccess(layerId, *cand);
                }
                if (oc.reader) {
                    src.reset();   // 区间打开成功：丢弃未消费的流（子经区间读取）
                    return std::move(oc.reader);
                }
                throw CorruptError(oc.failMsg);   // 区间推导误判等 → 回退 spool
            } catch (PasswordExhausted&) {
                throw;   // 密码耗尽：与 spool 路径数据相同，重试无意义
            } catch (CorruptError&) {
                // 回退 spool 原路径
            } catch (Error&) {
                // 码表探测等异常：回退
            }
        }
        auto spool = spool_all(*src, opt);
        src.reset();
        std::string cs = detect_zip_charset(
            [&] { return std::make_shared<SpoolSeekView>(spool); });
        std::unique_ptr<PushbackSource> nullSrc{};
        auto oc = try_open(fmt, nullSrc, spool, nullptr, nullptr, cs.c_str());
        if (oc.reader) return std::move(oc.reader);
        if (oc.fail == FailKind::Password)
            return password_loop(fmt, spool, layerId, pw, cs.c_str());
        throw CorruptError(oc.failMsg);
    }

    if (classify(fmt) == FormatClass::RandContainer) {
        // R 类：优先父区间直交 7z.dll（免 spool）；失败回退全量 spool
        if (region && (fmt == Format::SevenZip || fmt == Format::Rar) && sz::dll_available()) {
            try {
                std::map<std::wstring, sz::VolumeSource> vols;
                sz::VolumeSource v;
                v.region = region;
                vols[L""] = std::move(v);
                auto r = sz::open_archive(fmt, vols, L"", layerId, pw, opt);
                src.reset();
                return r;
            } catch (PasswordExhausted&) {
                throw;
            } catch (Error&) {
                // 回退 spool
            }
        }
        // R 类：先 spool 全量再随机访问
        auto spool = spool_all(*src, opt);
        src.reset();
        // 7z/rar：优先 7z.dll（全特性 + RAR 解码）；密码耗尽直抛，其余失败回退 libarchive
        if ((fmt == Format::SevenZip || fmt == Format::Rar) && sz::dll_available()) {
            try {
                std::map<std::wstring, sz::VolumeSource> vols;
                sz::VolumeSource v;
                v.spool = spool;
                v.winStart = 0;
                v.winLen = spool->size();
                vols[L""] = std::move(v);
                return sz::open_archive(fmt, vols, L"", layerId, pw, opt);
            } catch (PasswordExhausted&) {
                throw;
            } catch (Error&) {
                // 7z.dll 失败 → libarchive 兜底
            }
        }
        std::unique_ptr<PushbackSource> nullSrc{};
        auto oc = try_open(fmt, nullSrc, spool, nullptr);
        if (oc.reader) return std::move(oc.reader);
        if (oc.fail == FailKind::Password) {
            auto r = password_loop(fmt, spool, layerId, pw);
            return std::move(r);
        }
        throw CorruptError(oc.failMsg);
    }

    // S/Z 类：流式优先
    auto oc = try_open(fmt, src, nullptr, nullptr);
    if (oc.reader) return std::move(oc.reader);

    if (oc.fail == FailKind::Password) {
        // 加密层需要重启验证 → rewind + spool（D2 语义）
        src->rewindTo(0);
        auto spool = spool_all(*src, opt);
        src.reset();
        return password_loop(fmt, spool, layerId, pw);
    }
    if (oc.fail == FailKind::Corrupt) {
        // SFX / 追加修改 / 本地头流式盲区 → D2 回退 spool + 中央目录模式
        try {
            src->rewindTo(0);
        } catch (Error&)        {
            throw CorruptError("流式读取失败且回退窗口不足: " + oc.failMsg);
        }
        auto spool = spool_all(*src, opt);
        src.reset();
        std::unique_ptr<PushbackSource> nullSrc{};
        auto oc2 = try_open(fmt, nullSrc, spool, nullptr);
        if (oc2.reader) return std::move(oc2.reader);
        if (oc2.fail == FailKind::Password) {
            return password_loop(fmt, spool, layerId, pw);
        }
        throw CorruptError(oc2.failMsg);
    }
    throw CorruptError(oc.failMsg.empty() ? "容器打开失败" : oc.failMsg);
}

} // namespace nx
