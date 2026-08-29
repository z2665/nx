#include "stego.hpp"
#include "util.hpp"
#include <windows.h>
#include <cstring>
#include <vector>

namespace nx::stego {

namespace {

// 独立只读句柄：检测阶段不与引擎视图共享（open 交给各引擎自己的视图/计量）
struct FileReader {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t size = 0;

    explicit FileReader(const std::wstring& path) {
        h = CreateFileW(win_long_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(h, &sz))
            size = static_cast<uint64_t>(sz.QuadPart);
    }
    ~FileReader() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    bool ok() const { return h != INVALID_HANDLE_VALUE; }

    size_t read_at(uint64_t pos, void* buf, size_t n) {
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(pos);
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return 0;
        size_t got = 0;
        while (got < n) {
            DWORD r = 0;
            if (!ReadFile(h, static_cast<byte*>(buf) + got,
                          static_cast<DWORD>(n - got), &r, nullptr) || r == 0)
                break;
            got += r;
        }
        return got;
    }
};

uint32_t be32(const byte* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t be64(const byte* p) {
    return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4);
}

bool printable4(const byte* p) {
    for (int i = 0; i < 4; ++i)
        if (p[i] < 0x20 || p[i] > 0x7E) return false;
    return true;
}

// MP4 atom 步进：返回隐写候选起点；nullopt = 干净 MP4 / 不可判定
// （残尾不足一个原子头也按干净处理——不足承载任何归档魔数）
std::optional<uint64_t> mp4_walk(FileReader& f, uint64_t size) {
    if (size < 12) return std::nullopt;
    byte hdr[8];
    if (f.read_at(0, hdr, 8) != 8) return std::nullopt;
    if (std::memcmp(hdr + 4, "ftyp", 4) != 0) return std::nullopt;   // 仅标准 MP4
    uint64_t pos = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        if (pos == size) return std::nullopt;          // 原子精确吃完文件 → 干净
        if (pos + 8 > size) return std::nullopt;       // 残尾 <8B → 不足承载魔数
        if (f.read_at(pos, hdr, 8) != 8) return std::nullopt;
        uint64_t hdrSize = 8, sz = be32(hdr);
        if (sz == 1) {
            if (pos + 16 > size) return pos;           // 扩展长度头都放不下 → 隐写点
            byte ext[8];
            if (f.read_at(pos + 8, ext, 8) != 8) return std::nullopt;
            sz = be64(ext);
            hdrSize = 16;
        } else if (sz == 0) {
            return std::nullopt;                       // 该原子延伸到 EOF → 无尾部
        }
        if (sz < hdrSize) return pos;                  // 非法头 → 隐写点
        if (!printable4(hdr + 4)) return pos;          // 类型不可打印 → 隐写点
        if (pos + sz > size) return pos;               // 溢出文件 → 隐写点（或截断）
        pos += sz;
    }
    return std::nullopt;   // 原子数异常多（防御上限）
}

// EOCD 反向扫描：末 64KiB+22 内找 "PK\x05\x06"；真 EOCD 的注释必须精确吃到 EOF。
// 返回归档起始偏移（SFX 基址 = eocdPos - cdSize - cdOffset）；zip64 时返回 0。
std::optional<uint64_t> scan_eocd(FileReader& f, uint64_t size) {
    if (size < 22) return std::nullopt;
    const uint64_t win = 65535ull + 22 + 1;
    uint64_t start = size > win ? size - win : 0;
    size_t len = static_cast<size_t>(size - start);
    std::vector<byte> buf(len);
    if (f.read_at(start, buf.data(), len) != len) return std::nullopt;
    for (size_t off = len - 22 + 1; off-- > 0;) {   // 从尾往前
        if (!(buf[off] == 0x50 && buf[off + 1] == 0x4B && buf[off + 2] == 0x05 &&
              buf[off + 3] == 0x06))
            continue;
        uint16_t commentLen =
            static_cast<uint16_t>(buf[off + 20] | (buf[off + 21] << 8));
        uint64_t eocdAbs = start + off;
        if (eocdAbs + 22 + commentLen != size) continue;   // 必须精确到 EOF
        uint32_t cdSize = be32(&buf[off + 12]);
        uint32_t cdOff = be32(&buf[off + 16]);
        if (cdOff != 0xFFFFFFFF && cdOff + cdSize <= eocdAbs)
            return eocdAbs - cdSize - cdOff;               // SFX 基址
        return 0;   // zip64：起点交由引擎（libarchive）自行定位
    }
    return std::nullopt;
}

} // namespace

std::optional<Hit> scan(const std::wstring& path) {
    FileReader f(path);
    if (!f.ok() || f.size == 0) return std::nullopt;

    // ① MP4 原子步进：终点验归档魔数（7z/rar 的唯一发现途径）
    if (auto tail = mp4_walk(f, f.size)) {
        byte m[8];
        size_t got = f.read_at(*tail, m, 8);
        if (got >= 4 && m[0] == 'P' && m[1] == 'K' && m[2] == 0x03 && m[3] == 0x04)
            return Hit{*tail, Format::Zip, "mp4+zip"};
        if (got >= 6 && m[0] == '7' && m[1] == 'z' && m[2] == 0xBC && m[3] == 0xAF &&
            m[4] == 0x27 && m[5] == 0x1C)
            return Hit{*tail, Format::SevenZip, "mp4+7z"};
        if (got >= 7 && std::memcmp(m, "Rar!\x1A\x07", 6) == 0)
            return Hit{*tail, Format::Rar, (got >= 8 && m[7] == 1) ? "mp4+rar5" : "mp4+rar4"};
        // 原子终点无魔数：干净 MP4（mdat size=0 的尾藏 zip 仍可被 ② 兜住）
    }

    // ② EOCD 反扫：任意格式文件的尾接 zip
    if (auto base = scan_eocd(f, f.size))
        return Hit{*base, Format::Zip, "eocd"};
    return std::nullopt;
}

} // namespace nx::stego
