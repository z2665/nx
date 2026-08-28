#include "engines.hpp"
// ContainerReader/EngineOptions 契约见 container.hpp
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <cctype>
#include <deque>
#include <cstring>
#include <vector>

namespace nx {

namespace {

// ---- 错误分类（§6.2：必须区分"密码错"与"数据坏"）----
enum class FailKind { Password, Corrupt, Other };

FailKind classify_msg(const char* m) {
    if (!m) return FailKind::Other;
    std::string s(m);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s.find("passphrase") != std::string::npos || s.find("password") != std::string::npos)
        return FailKind::Password;
    if (s.find("crc") != std::string::npos || s.find("damaged") != std::string::npos ||
        s.find("truncat") != std::string::npos || s.find("invalid") != std::string::npos ||
        s.find("unrecognized") != std::string::npos || s.find("unsupported") != std::string::npos ||
        s.find("bad") != std::string::npos || s.find("corrupt") != std::string::npos)
        return FailKind::Corrupt;
    return FailKind::Other;
}

struct CbCtx {
    ByteSource* src = nullptr;               // 流式 = PushbackSource；spool = Reader
    SpoolBuffer::Reader* reader = nullptr;   // spool 模式提供随机访问
    std::vector<byte> buf;
};

la_ssize_t la_read_cb(archive*, void* c, const void** buf) {
    auto* ctx = static_cast<CbCtx*>(c);
    try {
        size_t n = ctx->src->read(ctx->buf);
        *buf = ctx->buf.data();
        return static_cast<la_ssize_t>(n);
    } catch (...) {
        return -1;
    }
}

la_int64_t la_seek_cb(archive*, void* c, la_int64_t off, int whence) {
    auto* ctx = static_cast<CbCtx*>(c);
    if (!ctx->reader) return -1;
    try {
        uint64_t size = ctx->reader->sizeHint().value_or(0);
        uint64_t pos = ctx->reader->pos();
        uint64_t abs = 0;
        switch (whence) {
            case SEEK_SET: abs = static_cast<uint64_t>(off); break;
            case SEEK_CUR: abs = pos + static_cast<uint64_t>(off); break;
            case SEEK_END: abs = size + static_cast<uint64_t>(off); break;
            default: return -1;
        }
        ctx->reader->seek(abs);
        return static_cast<la_int64_t>(abs);
    } catch (...) {
        return -1;
    }
}

// ---- LaSeqReader：libarchive 顺序条目读取器 ----

class LaSeqReader : public ContainerReader, public std::enable_shared_from_this<LaSeqReader> {
public:
    // borrowed（流式源借用探测）与 spool/reader（spool 模式）二选一；
    // 流式模式成功后由 caller 调 adoptStream() 过继所有权
    LaSeqReader(archive* a, archive_entry* e,
                PushbackSource* borrowed,
                std::shared_ptr<SpoolBuffer> spool,
                std::shared_ptr<SpoolBuffer::Reader> reader)
        : a_(a), e_(e, &archive_entry_free), borrowed_(borrowed),
          spool_(std::move(spool)), reader_(std::move(reader)) {
        ctx_.src = reader_ ? static_cast<ByteSource*>(reader_.get())
                           : static_cast<ByteSource*>(borrowed_);
        ctx_.reader = reader_.get();
        ctx_.buf.assign(64 << 10, 0);
    }

    ~LaSeqReader() override {
        if (a_) archive_read_free(a_);
    }

    // 流式模式成功确认后过继所有权（失败路径 caller 保留 src 以便 rewind/spool 回退）
    void adoptStream(std::unique_ptr<PushbackSource> s) {
        borrowed_ = nullptr;
        streamingSrc_ = std::move(s);
        ctx_.src = streamingSrc_.get();
    }

    CbCtx& ctx() { return ctx_; }
    archive* arch() { return a_; }

    bool next(ContainerEntry& out) override;

private:
    bool nextInternal(ContainerEntry& out);
public:

    size_t readEntryData(int idx, std::span<byte> buf);
    std::optional<uint64_t> entrySize(int idx) const {
        if (idx < 0 || idx >= static_cast<int>(sizes_.size())) return {};
        if (sizes_[idx] == UINT64_MAX) return {};
        return sizes_[idx];
    }

    // 探测：前进到首个含数据的条目并读首块（验证密码）。失败经异常（密码/损坏）。
    // 途经的目录/空文件条目进入 replayQ_ 由 next() 重放（顺序保持）；
    // 停在的条目预读字节存 probeFront_ 重放给下游（否则条目流短 1 字节）。
    // 7z 等工具会把目录条目放在最前——只看首条目会漏掉密码验证。
    void probeFirst() {
        for (;;) {
            ContainerEntry tmp;
            if (!nextInternal(tmp)) return;   // 空容器：open 本身即验证
            if (tmp.isSymlink) { replayQ_.push_back(tmp); continue; }
            if (tmp.isDir) { replayQ_.push_back(tmp); continue; }
            if (tmp.size == 0) {   // 空文件：无数据可验，换成空流重放
                tmp.data = std::make_shared<NullSource>();
                replayQ_.push_back(tmp);
                continue;
            }
            byte b[1];
            size_t got = readEntryData(curIdx_, std::span<byte>(b, 1));
            probeFront_.assign(b, b + got);
            replayQ_.push_back(tmp);
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
    std::shared_ptr<SpoolBuffer::Reader> reader_;
    CbCtx ctx_;
    std::deque<ContainerEntry> replayQ_;   // probe 预取条目重放队列
    std::vector<byte> probeFront_;         // probe 预读待重放字节
    int curIdx_ = -1;
    std::vector<byte> leftover_;
    size_t leftoverOff_ = 0;
    uint64_t entryPos_ = 0;      // 已拉入 leftover 的条目内偏移
    std::vector<uint64_t> sizes_;
};

class LaEntrySource : public ByteSource {
public:
    LaEntrySource(std::shared_ptr<LaSeqReader> r, int idx) : r_(std::move(r)), idx_(idx) {}
    size_t read(std::span<byte> buf) override { return r_->readEntryData(idx_, buf); }
    std::optional<uint64_t> sizeHint() const override { return r_->entrySize(idx_); }
private:
    std::shared_ptr<LaSeqReader> r_;
    int idx_;
};

bool LaSeqReader::next(ContainerEntry& out) {
    if (!replayQ_.empty()) {
        out = replayQ_.front();   // probe 途经/停留的条目按序重放
        replayQ_.pop_front();
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
    leftover_.clear();
    leftoverOff_ = 0;
    entryPos_ = 0;
    probeFront_.clear();
    const char* nm = archive_entry_pathname(e_.get());
    out.name = nm ? nm : "";
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

size_t LaSeqReader::readEntryData(int idx, std::span<byte> buf) {
    if (idx != curIdx_) throw Error("条目流已失效（迭代已前进）");
    if (buf.empty()) return 0;
    if (!probeFront_.empty()) {   // probe 预读字节优先交付
        size_t n = std::min(buf.size(), probeFront_.size());
        std::memcpy(buf.data(), probeFront_.data(), n);
        probeFront_.erase(probeFront_.begin(), probeFront_.begin() + n);
        return n;
    }
    if (leftoverOff_ < leftover_.size()) {
        size_t n = std::min(buf.size(), leftover_.size() - leftoverOff_);
        std::memcpy(buf.data(), leftover_.data() + leftoverOff_, n);
        leftoverOff_ += n;
        return n;
    }
    const void* p = nullptr;
    size_t sz = 0;
    la_int64_t off = 0;
    int r = archive_read_data_block(a_, &p, &sz, &off);
    if (r == ARCHIVE_EOF) return 0;
    const char* emsg = archive_error_string(a_);
    if (r == ARCHIVE_WARN) {
        std::string low = emsg ? emsg : "";
        for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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
    leftover_.assign(static_cast<const byte*>(p), static_cast<const byte*>(p) + sz);
    leftoverOff_ = 0;
    entryPos_ += sz;
    size_t n = std::min(buf.size(), leftover_.size());
    std::memcpy(buf.data(), leftover_.data(), n);
    leftoverOff_ = n;
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
                     const SecureStr* pw) {
    OpenOutcome oc;
    archive* a = nullptr;
    try {
        a = make_arch(fmt);
        if (pw && !pw->empty()) archive_read_add_passphrase(a, pw->c_str());
        std::shared_ptr<SpoolBuffer::Reader> reader;
        if (spool) reader = spool->reader();
        auto r = std::make_shared<LaSeqReader>(a, archive_entry_new(),
                                               spool ? nullptr : streamingSrc.get(),
                                               spool, std::move(reader));
        archive_read_set_read_callback(a, la_read_cb);
        archive_read_set_close_callback(a, [](archive*, void*) { return ARCHIVE_OK; });
        if (r->ctx().reader) archive_read_set_seek_callback(a, la_seek_cb);
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
        if (!spool) r->adoptStream(std::move(streamingSrc));   // 成功：过继
        oc.reader = std::move(r);
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
                                           PasswordProvider& pw) {
    for (;;) {
        auto cand = pw.nextAttempt(layerId);
        if (!cand)
            throw PasswordExhausted(layerId, "密码缺失或已耗尽: " + layerId);
        std::unique_ptr<PushbackSource> nullSrc{};
        auto oc = try_open(fmt, nullSrc, spool, &*cand);
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

std::shared_ptr<ContainerReader> open_container(std::unique_ptr<PushbackSource> src,
                                                Format fmt,
                                                const std::string& layerId,
                                                PasswordProvider& pw,
                                                const EngineOptions& opt) {
    if (classify(fmt) == FormatClass::RandContainer) {
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
