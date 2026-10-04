#include "detect.hpp"
#include <algorithm>
#include <cstring>

namespace nx {

namespace {

bool magic(std::span<const byte> p, const void* m, size_t n) {
    return p.size() >= n && std::memcmp(p.data(), m, n) == 0;
}

uint64_t parse_octal(const char* s, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\0') break;
        if (s[i] == ' ') continue;
        if (s[i] < '0' || s[i] > '9') return UINT64_MAX;   // 非法
        v = v * 8 + static_cast<uint64_t>(s[i] - '0');
    }
    return v;
}

// tar 结构校验：512 字节头 + offset 148 校验和（设计 §3.2）
bool looks_like_tar(std::span<const byte> p) {
    if (p.size() < 512) return false;
    const char* h = reinterpret_cast<const char*>(p.data());
    // 校验和字段本身按空格参与求和
    long long sum = 0, signedSum = 0;
    for (size_t i = 0; i < 512; ++i) {
        unsigned char c = static_cast<unsigned char>(h[i]);
        if (i >= 148 && i < 156) { sum += ' '; signedSum += ' '; }
        else { sum += c; signedSum += static_cast<signed char>(h[i]); }
    }
    uint64_t chk = parse_octal(h + 148, 8);
    if (chk == UINT64_MAX) return false;
    if (sum != static_cast<long long>(chk) && signedSum != static_cast<long long>(chk)) return false;
    // magic（ustar/pax/gnu 若存在必须合法）
    if (std::memcmp(h + 257, "ustar", 5) == 0) {
        // ustar\0 / ustar空间 / gnu 扩展
        if (!(h[262] == '\0' || h[262] == ' ' || std::memcmp(h + 257, "ustar  \0", 8) == 0))
            return false;
    }
    // size 字段须为合法八进制（base-256 扩展亦接受：首字节 0x80）
    if (!(static_cast<unsigned char>(h[124]) & 0x80)) {
        if (parse_octal(h + 124, 12) == UINT64_MAX) return false;
    }
    // 文件名主体大体可打印（前 32 字节）
    for (size_t i = 0; i < 32 && h[i]; ++i)
        if (static_cast<unsigned char>(h[i]) < 0x20) return false;
    return true;
}

// zip 本地头字段合法性（offset-0 与 SFX 扫描共用；原两处逐字重复）
bool valid_zip_lfh(const byte* q, size_t avail) {
    if (avail < 30) return false;
    unsigned method = q[8] | (q[9] << 8);
    unsigned nlen = q[26] | (q[27] << 8);
    unsigned elen = q[28] | (q[29] << 8);
    return method <= 99 && nlen > 0 && nlen < 4096 && elen < 65536;
}

} // namespace

// 纯核心：字节窗口 → 格式判定。所有 peek/refill 决策都在壳层
Detection detect_from_bytes(std::span<const byte> p, std::optional<uint64_t> sizeHint) {
    Detection d;
    if (p.empty()) return d;

    // ---- 精确 magic（顺序按特异性）----
    if (magic(p, "PK\x03\x04", 4)) {
        // 本地头字段合法性：压缩方法已知、文件名长度合理
        if (valid_zip_lfh(p.data(), p.size())) {
            unsigned method = p[8] | (p[9] << 8);
            d.fmt = Format::Zip; d.detail = (method == 0 ? "stored" : "deflate/其他");
            return d;
        }
        d.fmt = Format::Zip; return d;   // 字段异常也按 zip 交给引擎报错
    }
    if (magic(p, "PK\x05\x06", 4) || magic(p, "PK\x07\x08", 4)) { d.fmt = Format::Zip; return d; }
    if (magic(p, "7z\xBC\xAF\x27\x1C", 6)) { d.fmt = Format::SevenZip; return d; }
    if (magic(p, "Rar!\x1A\x07\x01\x00", 8)) { d.fmt = Format::Rar; d.detail = "rar5"; return d; }
    if (magic(p, "Rar!\x1A\x07\x00", 7)) { d.fmt = Format::Rar; d.detail = "rar4"; return d; }
    if (magic(p, "\x1F\x8B\x08", 3)) {
        if (p.size() >= 4 && (p[3] & 0xE0) == 0) { d.fmt = Format::Gzip; return d; }
    }
    if (magic(p, "\xFD\x37\x7A\x58\x5A\x00", 6)) { d.fmt = Format::Xz; return d; }
    if (magic(p, "BZh", 3)) {
        // 块魔数 0x314159265359（pi）在偏移 4，共 6 字节
        if (p.size() >= 10 && p[3] >= '1' && p[3] <= '9' &&
            magic(p.subspan(4), "1AY&SY", 6)) {
            d.fmt = Format::Bzip2; return d;
        }
    }
    if (magic(p, "\x28\xB5\x2F\xFD", 4)) { d.fmt = Format::Zstd; return d; }
    if (magic(p, "\x04\x22\x4D\x18", 4)) { d.fmt = Format::Lz4; return d; }
    if (magic(p, "\x1F\x9D", 2)) {
        if (p.size() >= 3 && p[2] <= 16) { d.fmt = Format::CompressZ; return d; }
    }
    if (magic(p, "\x5D\x00\x00", 3)) {
        // 裸 lzma：属性字节 5D + 字典大小字段大体合法（非全零）
        if (p.size() >= 13 && (p[3] != 0 || p[4] != 0 || p[5] != 0 || p[6] != 0)) {
            d.fmt = Format::Lzma; return d;
        }
    }
    if (magic(p, "070707", 6) || magic(p, "070701", 6) || magic(p, "070702", 6)) {
        d.fmt = Format::Cpio; return d;
    }
    if (magic(p, "!<arch>\n", 8)) { d.fmt = Format::Ar; return d; }
    if (magic(p, "MSCF", 4)) { d.fmt = Format::Cab; return d; }
    if (magic(p, "MSWIM\x00\x00\x00", 8)) { d.fmt = Format::Wim; return d; }

    // ---- tar：offset 257 无固定 magic，靠校验和 ----
    if (looks_like_tar(p)) { d.fmt = Format::Tar; return d; }

    // ---- SFX 前缀扫描（D1）：PE/安装器前缀内寻找 zip/7z/rar 魔数（窗口 ≈4MiB）----
    {
        size_t scanEnd = std::min<size_t>(p.size(), 4 << 20);
        for (size_t i = 0; i + 4 <= scanEnd; ++i) {
            const byte* q = p.data() + i;
            if (q[0] == 'P' && q[1] == 'K' && q[2] == 3 && q[3] == 4 && i + 30 <= scanEnd) {
                if (valid_zip_lfh(q, scanEnd - i)) {
                    d.fmt = Format::Zip;
                    d.sfxOffset = i;
                    return d;
                }
            }
            if (i + 6 <= scanEnd && q[0] == '7' && q[1] == 'z' && q[2] == 0xBC && q[3] == 0xAF &&
                q[4] == 0x27 && q[5] == 0x1C) {
                d.fmt = Format::SevenZip;
                d.sfxOffset = i;
                return d;
            }
            if (i + 8 <= scanEnd && q[0] == 'R' && q[1] == 'a' && q[2] == 'r' && q[3] == '!' &&
                q[4] == 0x1A) {
                d.fmt = Format::Rar;
                d.detail = q[7] == 1 ? "rar5" : "rar4";
                d.sfxOffset = i;
                return d;
            }
        }
    }

    // ---- iso9660：magic 在 0x8001，需要 32KiB+6 窗口 ----
    if ((!sizeHint || *sizeHint >= 0x8006)) {
        if (p.size() >= 0x8006 && std::memcmp(p.data() + 0x8001, "CD001", 5) == 0) {
            d.fmt = Format::Iso; return d;
        }
    }

    // brotli 无 magic（设计 §3.1）——不支持试探解码
    return d;
}

// 壳：决定窥探窗口——64KiB 首扫；未命中且窗口未达 SFX 扫描上限（4MiB）→ 补拉重扫
Detection detect(PushbackSource& src, const std::string& nameHint) {
    (void)nameHint;   // 内容优先；扩展名仅分片排序用（volumeset）
    auto p = src.peek(64 << 10);
    Detection d = detect_from_bytes(p, src.sizeHint());
    if (d.fmt != Format::Unknown) return d;
    if (p.size() < (4 << 20)) {
        p = src.peek(4 << 20);   // 小窗未命中且流可能更长（含 iso 的 0x8006 偏移）
        d = detect_from_bytes(p, src.sizeHint());
    }
    return d;
}

} // namespace nx
