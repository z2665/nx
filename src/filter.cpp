#include "filter.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <zlib.h>
#include <bzlib.h>
#include <lzma.h>
#include <zstd.h>
#include <lz4frame.h>
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace nx {

namespace {

constexpr size_t kInBlock = 256 << 10;    // 输入块
constexpr size_t kOutBlock = 256 << 10;   // 输出块

// 通用压缩输入器：按需从 PushbackSource 拉取（字节全部经过历史窗口，
// 供多成员窥探使用）
class CompressedIn {
public:
    explicit CompressedIn(PushbackSource& s) : src_(s) { buf_.resize(kInBlock); }
    // 返回当前可用输入（可能为空 → refill）
    const byte* data() const { return buf_.data() + off_; }
    size_t avail() const { return have_ - off_; }
    void consume(size_t n) { off_ += n; }
    bool refill() {
        if (off_ > 0) {
            std::memmove(buf_.data(), buf_.data() + off_, have_ - off_);
            have_ -= off_;
            off_ = 0;
        }
        if (have_ == buf_.size()) return true;
        size_t got = src_.read(std::span<byte>(buf_.data() + have_, buf_.size() - have_));
        have_ += got;
        return got > 0;
    }
    bool atEof() {
        return avail() == 0 && !refill();
    }
    PushbackSource& src_;
    std::vector<byte> buf_;
    size_t off_ = 0, have_ = 0;
};

// 窥探 in 缓冲内剩余字节是否为同格式下一成员的开头
// （成员边界必须在 in 缓冲内判断——这些字节已从 src 消费，src 的 peek 看不到它们）
bool next_member_is(CompressedIn& in, Format fmt) {
    if (in.avail() == 0 && !in.refill()) return false;
    const byte* q = in.data();
    size_t n = in.avail();
    auto eq = [&](const void* m, size_t k) { return n >= k && std::memcmp(q, m, k) == 0; };
    switch (fmt) {
        case Format::Gzip:  return eq("\x1F\x8B", 2);
        case Format::Bzip2: return eq("BZh", 3);
        case Format::Xz:    return eq("\xFD\x37\x7A\x58\x5A\x00", 6);
        case Format::Lzma:  return eq("\x5D\x00\x00", 3);
        case Format::Zstd:  return eq("\x28\xB5\x2F\xFD", 4);
        case Format::Lz4:   return eq("\x04\x22\x4D\x18", 4);
        default:            return false;
    }
}

// 尾部垃圾检测：允许全零填充（部分工具在 gzip 后补零）
bool trailing_all_zero(CompressedIn& in) {
    while (in.avail() > 0) {
        for (size_t i = 0; i < in.avail(); ++i)
            if (in.data()[i] != 0) return false;
        in.consume(in.avail());
        if (!in.refill()) return true;
    }
    return true;
}

// ---- 五家 C API 的 RAII 适配（M2：init/end 手工清理散布 20+ 出口点 → 构造/析构；
// 原实现连 zstd 初始化失败路径都漏 free）----
// 三段式多成员协议（gzip/bzip2/xz·lzma 同构）：bind → step → 产出/消费，
// 成员尾窥探下一成员 magic → reset 或收尾
struct ZlibDec {
    static constexpr const char* kName = "gzip";
    z_stream s{};
    ZlibDec() {
        if (inflateInit2(&s, 15 + 16) != Z_OK) throw Error("zlib 初始化失败");
    }
    ~ZlibDec() { inflateEnd(&s); }
    void bindIn(const byte* p, size_t n) {
        s.next_in = const_cast<Bytef*>(p);
        s.avail_in = static_cast<uInt>(n);
    }
    void bindOut(byte* p, size_t n) { s.next_out = p; s.avail_out = static_cast<uInt>(n); }
    int step() { return inflate(&s, Z_NO_FLUSH); }
    size_t availIn() const { return s.avail_in; }
    size_t produced(size_t cap) const { return cap - s.avail_out; }
    static bool end(int r) { return r == Z_STREAM_END; }
    static bool ok(int r) { return r == Z_OK; }
    std::string errMsg(int) const {
        return std::string(kName) + " 数据损坏 (" + (s.msg ? s.msg : "zlib") + ")";
    }
    void reset() {
        if (inflateReset(&s) != Z_OK) throw Error("zlib 重置失败");
    }
};

struct Bzip2Dec {
    static constexpr const char* kName = "bzip2";
    bz_stream s{};
    Bzip2Dec() { init("初始化"); }
    ~Bzip2Dec() {
        if (armed) BZ2_bzDecompressEnd(&s);
    }
    void bindIn(const byte* p, size_t n) {
        s.next_in = reinterpret_cast<char*>(const_cast<byte*>(p));
        s.avail_in = static_cast<unsigned>(n);
    }
    void bindOut(byte* p, size_t n) {
        s.next_out = reinterpret_cast<char*>(p);
        s.avail_out = static_cast<unsigned>(n);
    }
    int step() { return BZ2_bzDecompress(&s); }
    size_t availIn() const { return s.avail_in; }
    size_t produced(size_t cap) const { return cap - s.avail_out; }
    static bool end(int r) { return r == BZ_STREAM_END; }
    static bool ok(int r) { return r == BZ_OK; }
    std::string errMsg(int r) const {
        return std::string(kName) + " 数据损坏 (bzlib " + std::to_string(r) + ")";
    }
    void reset() {   // bzlib 无轻量重置：end 后重 init
        BZ2_bzDecompressEnd(&s);
        armed = false;
        init("重置");
    }
private:
    void init(const char* what) {
        if (BZ2_bzDecompressInit(&s, 0, 0) != BZ_OK)
            throw Error(std::string("bzip2 ") + what + "失败");
        armed = true;
    }
    bool armed = false;
};

struct LzmaDec {
    static constexpr const char* kName = "xz/lzma";
    lzma_stream s = LZMA_STREAM_INIT;
    LzmaDec() { init("初始化"); }
    ~LzmaDec() {
        if (armed) lzma_end(&s);
    }
    void bindIn(const byte* p, size_t n) { s.next_in = p; s.avail_in = n; }
    void bindOut(byte* p, size_t n) { s.next_out = p; s.avail_out = n; }
    int step() { return static_cast<int>(lzma_code(&s, LZMA_RUN)); }
    size_t availIn() const { return s.avail_in; }
    size_t produced(size_t cap) const { return cap - s.avail_out; }
    static bool end(int r) { return r == LZMA_STREAM_END; }
    static bool ok(int r) { return r == LZMA_OK; }
    std::string errMsg(int r) const {
        return std::string(kName) + " 数据损坏 (liblzma " + std::to_string(r) + ")";
    }
    void reset() {   // liblzma 无重置：end 后重建
        lzma_end(&s);
        armed = false;
        s = LZMA_STREAM_INIT;
        init("重置");
    }
private:
    void init(const char* what) {
        if (lzma_auto_decoder(&s, UINT64_MAX, 0) != LZMA_OK)
            throw Error(std::string("liblzma ") + what + "失败");
        armed = true;
    }
    bool armed = false;
};

struct ZstdDec {
    ZSTD_DStream* d;
    ZstdDec() : d(ZSTD_createDStream()) {
        if (!d || ZSTD_initDStream(d) != 0) throw Error("zstd 初始化失败");
    }
    ~ZstdDec() {
        if (d) ZSTD_freeDStream(d);
    }
    ZstdDec(const ZstdDec&) = delete;
    ZstdDec& operator=(const ZstdDec&) = delete;
};

struct Lz4Dec {
    LZ4F_dctx* d = nullptr;
    Lz4Dec() {
        if (LZ4F_createDecompressionContext(&d, LZ4F_VERSION) != 0)
            throw Error("lz4 初始化失败");
    }
    ~Lz4Dec() {
        if (d) LZ4F_freeDecompressionContext(d);
    }
    Lz4Dec(const Lz4Dec&) = delete;
    Lz4Dec& operator=(const Lz4Dec&) = delete;
};

// 三段式多成员解码主循环（gzip/bzip2/xz·lzma 同构；M2 模板合并——三段 ~40 行
// 逐字重复的多成员循环归一，异常/弃置路径的释放全部交给 Dec 的 RAII）
template <class Dec, class Emit>
void pump_members(Format fmt, CompressedIn& ci, std::vector<byte>& obuf, Emit&& emit) {
    Dec d;
    bool done = false;
    while (!done) {
        int r = 0;   // Z_OK/BZ_OK/LZMA_OK == 0
        for (;;) {
            if (ci.avail() == 0 && !ci.refill()) {
                if (!Dec::end(r)) throw CorruptError(std::string(Dec::kName) + " 流截断");
                break;
            }
            d.bindIn(ci.data(), ci.avail());
            d.bindOut(obuf.data(), obuf.size());
            r = d.step();
            ci.consume(ci.avail() - d.availIn());
            size_t got = d.produced(obuf.size());
            if (got > 0 && !emit(obuf.data(), got)) return;
            if (Dec::end(r)) break;
            if (!Dec::ok(r)) throw CorruptError(d.errMsg(r));
        }
        if (next_member_is(ci, fmt)) {
            d.reset();
        } else {
            if (ci.avail() != 0 && !trailing_all_zero(ci))
                throw CorruptError(std::string(Dec::kName) + " 流后存在无法解析的多余字节");
            done = true;
        }
    }
}

} // namespace

void filter_decode(Format fmt, PushbackSource& in, BoundedQueue<std::vector<byte>>& out,
                   std::exception_ptr& err, const FilterLimiter& lim) {
    try {
        CompressedIn ci(in);
        std::vector<byte> obuf(kOutBlock);
        auto emit = [&](const byte* p, size_t n) -> bool {
            if (n == 0) return true;
            if (lim.produced) {
                uint64_t total = lim.produced->fetch_add(n, std::memory_order_relaxed) + n;
                if (lim.maxRatio && lim.inputBytes && lim.limitTripped) {
                    // 分母 = max(实时输入, 根尺寸提示)：小输入炸弹（如 10KB gz→10MB）也能判定
                    uint64_t inB = std::max(lim.inputBytes->load(std::memory_order_relaxed),
                                            lim.inputFloor);
                    if (inB >= (16 << 10) && total > inB * lim.maxRatio) {
                        lim.limitTripped->store(true);
                        throw LimitError("压缩比超过熔断上限 " + std::to_string(lim.maxRatio) +
                                         "（已产出 " + std::to_string(total) + " / 输入 " +
                                         std::to_string(inB) + "）");
                    }
                }
            }
            std::vector<byte> blk(p, p + n);
            return out.push(std::move(blk));
        };

        if (fmt == Format::Gzip) {
            pump_members<ZlibDec>(fmt, ci, obuf, emit);
        }
        else if (fmt == Format::Bzip2) {
            pump_members<Bzip2Dec>(fmt, ci, obuf, emit);
        }
        else if (fmt == Format::Xz || fmt == Format::Lzma) {
            pump_members<LzmaDec>(fmt, ci, obuf, emit);
        }
        else if (fmt == Format::Zstd) {
            ZstdDec d;
            // zstd 解码器原生支持多 frame 串联，无需重启
            bool eof = false;
            while (!eof) {
                if (ci.avail() == 0 && !ci.refill()) break;
                const byte* pin = ci.data();
                size_t pinLen = ci.avail();
                size_t consumedTotal = 0;
                bool inputDone = false;
                while (!inputDone) {
                    ZSTD_inBuffer inb{pin + consumedTotal, pinLen - consumedTotal, 0};
                    ZSTD_outBuffer outb{obuf.data(), obuf.size(), 0};
                    size_t r = ZSTD_decompressStream(d.d, &outb, &inb);
                    if (ZSTD_isError(r))
                        throw CorruptError(std::string("zstd 数据损坏 (") +
                                           ZSTD_getErrorName(r) + ")");
                    if (outb.pos > 0 && !emit(obuf.data(), outb.pos)) return;
                    consumedTotal += inb.pos;
                    if (consumedTotal >= pinLen) inputDone = true;
                    else if (inb.pos == 0 && outb.pos < outb.size)
                        throw CorruptError("zstd 无进展（数据损坏）");
                }
                ci.consume(consumedTotal);
                if (ci.avail() == 0 && !ci.refill()) eof = true;
            }
        }
        else if (fmt == Format::Lz4) {
            Lz4Dec d;
            bool eof = false;
            while (!eof) {
                if (ci.avail() == 0 && !ci.refill()) break;
                size_t inLen = ci.avail();
                for (;;) {
                    size_t outLen = obuf.size();
                    size_t consumed = inLen;
                    size_t r = LZ4F_decompress(d.d, obuf.data(), &outLen, ci.data(), &consumed,
                                               nullptr);
                    if (LZ4F_isError(r))
                        throw CorruptError(std::string("lz4 数据损坏 (") +
                                           LZ4F_getErrorName(r) + ")");
                    ci.consume(consumed);
                    inLen = ci.avail();
                    if (outLen > 0 && !emit(obuf.data(), outLen)) return;
                    if (consumed == 0 && outLen == 0) break;   // 无进展（输入耗尽）
                    if (inLen == 0) break;
                }
                if (ci.avail() == 0 && !ci.refill()) eof = true;
            }
        }
        else if (fmt == Format::CompressZ) {
            decode_via_libarchive(in, [&](std::span<const byte> p) {
                return emit(p.data(), p.size());
            }, Format::CompressZ);
        }
        else {
            throw Error("不支持的过滤器: " + std::string(format_name(fmt)));
        }
    } catch (...) {
        err = std::current_exception();
        out.close();
    }
    out.close();
}

// ---------- .Z via libarchive（raw + compress filter，单成员） ----------

namespace {
struct LaCbCtx { PushbackSource* src; std::vector<byte> buf; };

la_ssize_t la_read_cb(archive*, void* c, const void** buf) {
    auto* ctx = static_cast<LaCbCtx*>(c);
    *buf = ctx->buf.data();
    return static_cast<la_ssize_t>(ctx->src->read(ctx->buf));
}
} // namespace

void decode_via_libarchive(PushbackSource& in,
                           const std::function<bool(std::span<const byte>)>& emit,
                           Format filterAs) {
    (void)filterAs;   // 调用方语义标注（.Z 专用）；实现走 filter_all+raw
    archive* a = archive_read_new();
    if (!a) throw Error("libarchive 分配失败");
    archive_read_support_filter_all(a);
    archive_read_support_format_raw(a);
    LaCbCtx ctx{&in, std::vector<byte>(64 << 10)};
    if (archive_read_open(a, &ctx, nullptr, la_read_cb, nullptr) != ARCHIVE_OK) {
        std::string m = archive_error_string(a) ? archive_error_string(a) : "open 失败";
        archive_read_free(a);
        throw CorruptError(".Z 打开失败: " + m);
    }
    archive_entry* e = nullptr;
    int r = archive_read_next_header(a, &e);
    if (r != ARCHIVE_OK) {
        std::string m = archive_error_string(a) ? archive_error_string(a) : "读取失败";
        archive_read_free(a);
        throw CorruptError(".Z 读取失败: " + m);
    }
    for (;;) {
        const void* p = nullptr;
        size_t sz = 0;
        la_int64_t off = 0;
        r = archive_read_data_block(a, &p, &sz, &off);
        if (r == ARCHIVE_EOF) break;
        if (r != ARCHIVE_OK) {
            std::string m = archive_error_string(a) ? archive_error_string(a) : "数据错误";
            archive_read_free(a);
            throw CorruptError(".Z 数据损坏: " + m);
        }
        if (!emit(std::span<const byte>(static_cast<const byte*>(p), sz))) break;
    }
    archive_read_free(a);
}

} // namespace nx
