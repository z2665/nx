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

} // namespace

void filter_decode(Format fmt, PushbackSource& in, BoundedQueue<std::vector<byte>>& out,
                   std::exception_ptr& err) {
    try {
        CompressedIn ci(in);
        std::vector<byte> obuf(kOutBlock);
        auto emit = [&](const byte* p, size_t n) -> bool {
            std::vector<byte> blk(p, p + n);
            return out.push(std::move(blk));
        };

        if (fmt == Format::Gzip) {
            z_stream s{};
            if (inflateInit2(&s, 15 + 16) != Z_OK) throw Error("zlib 初始化失败");
            bool done = false;
            while (!done) {
                int r = Z_OK;
                // 一个成员：直到 Z_STREAM_END
                for (;;) {
                    if (ci.avail() == 0 && !ci.refill()) {
                        if (r != Z_STREAM_END) { inflateEnd(&s); throw CorruptError("gzip 流截断"); }
                        break;
                    }
                    s.next_in = const_cast<Bytef*>(ci.data());
                    s.avail_in = static_cast<uInt>(ci.avail());
                    s.next_out = obuf.data();
                    s.avail_out = static_cast<uInt>(obuf.size());
                    r = inflate(&s, Z_NO_FLUSH);
                    ci.consume(ci.avail() - s.avail_in);
                    size_t got = obuf.size() - s.avail_out;
                    if (got > 0 && !emit(obuf.data(), got)) { inflateEnd(&s); return; }
                    if (r == Z_STREAM_END) break;
                    if (r != Z_OK) {
                        inflateEnd(&s);
                        throw CorruptError(std::string("gzip 数据损坏 (") + (s.msg ? s.msg : "zlib") + ")");
                    }
                }
                // 多成员：窥探
                if (next_member_is(ci, Format::Gzip)) {
                    if (inflateReset(&s) != Z_OK) { inflateEnd(&s); throw Error("zlib 重置失败"); }
                } else {
                    if (ci.avail() != 0 && !trailing_all_zero(ci)) {
                        inflateEnd(&s);
                        throw CorruptError("gzip 流后存在无法解析的多余字节");
                    }
                    done = true;
                }
            }
            inflateEnd(&s);
        }
        else if (fmt == Format::Bzip2) {
            bz_stream s{};
            if (BZ2_bzDecompressInit(&s, 0, 0) != BZ_OK) throw Error("bzip2 初始化失败");
            bool done = false;
            while (!done) {
                int r = BZ_OK;
                for (;;) {
                    if (ci.avail() == 0 && !ci.refill()) {
                        if (r != BZ_STREAM_END) { BZ2_bzDecompressEnd(&s); throw CorruptError("bzip2 流截断"); }
                        break;
                    }
                    s.next_in = reinterpret_cast<char*>(const_cast<byte*>(ci.data()));
                    s.avail_in = static_cast<unsigned>(ci.avail());
                    s.next_out = reinterpret_cast<char*>(obuf.data());
                    s.avail_out = static_cast<unsigned>(obuf.size());
                    r = BZ2_bzDecompress(&s);
                    ci.consume(ci.avail() - s.avail_in);
                    size_t got = obuf.size() - s.avail_out;
                    if (got > 0 && !emit(obuf.data(), got)) { BZ2_bzDecompressEnd(&s); return; }
                    if (r == BZ_STREAM_END) break;
                    if (r != BZ_OK) {
                        int code = r;
                        BZ2_bzDecompressEnd(&s);
                        throw CorruptError("bzip2 数据损坏 (bzlib " + std::to_string(code) + ")");
                    }
                }
                if (next_member_is(ci, Format::Bzip2)) {
                    BZ2_bzDecompressEnd(&s);
                    if (BZ2_bzDecompressInit(&s, 0, 0) != BZ_OK) throw Error("bzip2 重置失败");
                } else {
                    if (ci.avail() != 0 && !trailing_all_zero(ci)) {
                        BZ2_bzDecompressEnd(&s);
                        throw CorruptError("bzip2 流后存在无法解析的多余字节");
                    }
                    done = true;
                }
            }
            BZ2_bzDecompressEnd(&s);
        }
        else if (fmt == Format::Xz || fmt == Format::Lzma) {
            lzma_stream s = LZMA_STREAM_INIT;
            if (lzma_auto_decoder(&s, UINT64_MAX, 0) != LZMA_OK) throw Error("liblzma 初始化失败");
            bool done = false;
            while (!done) {
                lzma_ret r = LZMA_OK;
                for (;;) {
                    if (ci.avail() == 0 && !ci.refill()) {
                        if (r != LZMA_STREAM_END) { lzma_end(&s); throw CorruptError("xz/lzma 流截断"); }
                        break;
                    }
                    s.next_in = ci.data();
                    s.avail_in = ci.avail();
                    s.next_out = obuf.data();
                    s.avail_out = obuf.size();
                    r = lzma_code(&s, LZMA_RUN);
                    ci.consume(ci.avail() - s.avail_in);
                    size_t got = obuf.size() - s.avail_out;
                    if (got > 0 && !emit(obuf.data(), got)) { lzma_end(&s); return; }
                    if (r == LZMA_STREAM_END) break;
                    if (r != LZMA_OK) {
                        std::string m = "xz/lzma 数据损坏 (liblzma ";
                        m += std::to_string(static_cast<int>(r)); m += ")";
                        lzma_end(&s);
                        throw CorruptError(std::move(m));
                    }
                }
                if (next_member_is(ci, fmt)) {
                    lzma_end(&s);
                    s = LZMA_STREAM_INIT;
                    if (lzma_auto_decoder(&s, UINT64_MAX, 0) != LZMA_OK) throw Error("liblzma 重置失败");
                } else {
                    if (ci.avail() != 0 && !trailing_all_zero(ci)) {
                        lzma_end(&s);
                        throw CorruptError("xz/lzma 流后存在无法解析的多余字节");
                    }
                    done = true;
                }
            }
            lzma_end(&s);
        }
        else if (fmt == Format::Zstd) {
            ZSTD_DStream* d = ZSTD_createDStream();
            if (!d || ZSTD_initDStream(d) != 0) throw Error("zstd 初始化失败");
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
                    size_t r = ZSTD_decompressStream(d, &outb, &inb);
                    if (ZSTD_isError(r)) {
                        std::string m = std::string("zstd 数据损坏 (") + ZSTD_getErrorName(r) + ")";
                        ZSTD_freeDStream(d);
                        throw CorruptError(std::move(m));
                    }
                    if (outb.pos > 0 && !emit(obuf.data(), outb.pos)) { ZSTD_freeDStream(d); return; }
                    consumedTotal += inb.pos;
                    if (consumedTotal >= pinLen) inputDone = true;
                    else if (inb.pos == 0 && outb.pos < outb.size) {
                        ZSTD_freeDStream(d);
                        throw CorruptError("zstd 无进展（数据损坏）");
                    }
                }
                ci.consume(consumedTotal);
                if (ci.avail() == 0 && !ci.refill()) eof = true;
            }
            ZSTD_freeDStream(d);
        }
        else if (fmt == Format::Lz4) {
            LZ4F_dctx* d = nullptr;
            if (LZ4F_createDecompressionContext(&d, LZ4F_VERSION) != 0) throw Error("lz4 初始化失败");
            bool eof = false;
            while (!eof) {
                if (ci.avail() == 0 && !ci.refill()) break;
                size_t inLen = ci.avail();
                for (;;) {
                    size_t outLen = obuf.size();
                    size_t consumed = inLen;
                    size_t r = LZ4F_decompress(d, obuf.data(), &outLen, ci.data(), &consumed, nullptr);
                    if (LZ4F_isError(r)) {
                        std::string m = std::string("lz4 数据损坏 (") + LZ4F_getErrorName(r) + ")";
                        LZ4F_freeDecompressionContext(d);
                        throw CorruptError(std::move(m));
                    }
                    ci.consume(consumed);
                    inLen = ci.avail();
                    if (outLen > 0 && !emit(obuf.data(), outLen)) { LZ4F_freeDecompressionContext(d); return; }
                    if (consumed == 0 && outLen == 0) break;   // 无进展（输入耗尽）
                    if (inLen == 0) break;
                }
                if (ci.avail() == 0 && !ci.refill()) eof = true;
            }
            LZ4F_freeDecompressionContext(d);
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
