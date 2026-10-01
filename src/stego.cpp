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
        AtomExtent ax;
        if (be32(hdr) == 1) {
            if (pos + 16 > size) return pos;           // 扩展长度头都放不下 → 隐写点
            byte ext[8];
            if (f.read_at(pos + 8, ext, 8) != 8) return std::nullopt;
            ax = parse_atom_header(hdr, ext);
        } else {
            ax = parse_atom_header(hdr);
        }
        if (ax.toEof) return std::nullopt;             // 该原子延伸到 EOF → 无尾部
        if (ax.atomSize < ax.hdrSize) return pos;      // 非法头 → 隐写点
        if (!printable4(hdr + 4)) return pos;          // 类型不可打印 → 隐写点
        if (pos + ax.atomSize > size) return pos;      // 溢出文件 → 隐写点（或截断）
        pos += ax.atomSize;
    }
    return std::nullopt;   // 原子数异常多（防御上限）
}

// EOCD 反向扫描（壳）：读窗口 → 纯解析（eocd_from_window）→ CD 自证（4B 文件读）。
// 不要求精确到 EOF——真实隐写样本在 EOCD 后拖伪装数据（含末尾假 mdat 原子）。
// ⚠ 自证不可省：zip64 档（D:\…\1.mp4 实测）的经典 EOCD 是影子值（cdOffset/cdSize
// 与真实目录不符，真值在 EOCD64），数学结果会偏移——自证不匹配则判定不可信
// （调用方回退魔数锚点窗口，libarchive 自会用 EOCD64 真值定位）。
struct EocdExtent {
    uint64_t base;
    uint64_t len;
};

std::optional<EocdExtent> scan_eocd(FileReader& f, uint64_t size) {
    if (size < 22) return std::nullopt;
    const uint64_t win = 65535ull + 22 + 1;
    uint64_t start = size > win ? size - win : 0;
    size_t len = static_cast<size_t>(size - start);
    std::vector<byte> buf(len);
    if (f.read_at(start, buf.data(), len) != len) return std::nullopt;
    EocdScanResult r = eocd_from_window(std::span<const byte>(buf), start, size);
    if (r.kind != EocdScanResult::Kind::Candidate) return std::nullopt;
    byte sig[4];   // 自证：CD 末条目签名（读失败/不匹配 = 影子值 → 不信任数学）
    if (f.read_at(r.cand.cdVerifyPos, sig, 4) != 4 ||
        std::memcmp(sig, "PK\x01\x02", 4) != 0)
        return std::nullopt;
    return EocdExtent{r.cand.base, r.cand.len};
}

} // namespace

// 纯核心：EOCD 反扫解析（窗口内从后往前，最近优先；字段小端）。
// None = 窗口无合法 EOCD；Candidate = 数学成立（自证由壳层读文件完成）；
// Distrust = zip64 标记 / 空目录——数学不可信，调用方应整体放弃
EocdScanResult eocd_from_window(std::span<const byte> win, uint64_t winStart,
                                uint64_t fileSize) {
    EocdScanResult r;
    if (win.size() < 22) return r;
    for (size_t off = win.size() - 22 + 1; off-- > 0;) {
        if (!(win[off] == 0x50 && win[off + 1] == 0x4B && win[off + 2] == 0x05 &&
              win[off + 3] == 0x06))
            continue;
        uint16_t commentLen =
            static_cast<uint16_t>(win[off + 20] | (win[off + 21] << 8));
        uint64_t eocdAbs = winStart + off;
        uint64_t eocdEnd = eocdAbs + 22 + commentLen;
        if (eocdEnd > fileSize) continue;                // 注释越界 = 非法
        uint32_t cdSize = le32(&win[off + 12]);          // 小端！
        uint32_t cdOff = le32(&win[off + 16]);
        if (cdOff == 0xFFFFFFFF) {                       // zip64 标记：基址不可反推
            r.kind = EocdScanResult::Kind::Distrust;
            return r;
        }
        uint64_t cdEnd = static_cast<uint64_t>(cdOff) + cdSize;
        if (cdEnd > eocdAbs) continue;                   // CD 必须在 EOCD 之前
        if (cdSize == 0) {                               // 空目录 → 不信任数学
            r.kind = EocdScanResult::Kind::Distrust;
            return r;
        }
        r.kind = EocdScanResult::Kind::Candidate;
        r.cand.base = eocdAbs - cdEnd;
        r.cand.len = eocdEnd - r.cand.base;
        r.cand.cdVerifyPos = eocdAbs - cdSize;
        return r;
    }
    return r;
}

std::optional<Hit> scan(const std::wstring& path) {
    FileReader f(path);
    if (!f.ok() || f.size == 0) return std::nullopt;

    // ① MP4 原子步进：终点验归档魔数（7z/rar 的唯一发现途径）
    bool mp4Zip = false;
    uint64_t mp4Off = 0;
    std::string mp4Desc;
    if (auto tail = mp4_walk(f, f.size)) {
        byte m[8];
        size_t got = f.read_at(*tail, m, 8);
        if (got >= 4 && m[0] == 'P' && m[1] == 'K' && m[2] == 0x03 && m[3] == 0x04) {
            mp4Zip = true;
            mp4Off = *tail;
            mp4Desc = "mp4+zip";
        } else if (got >= 6 && m[0] == '7' && m[1] == 'z' && m[2] == 0xBC && m[3] == 0xAF &&
                   m[4] == 0x27 && m[5] == 0x1C) {
            return Hit{*tail, 0, Format::SevenZip, "mp4+7z"};
        } else if (got >= 7 && std::memcmp(m, "Rar!\x1A\x07", 6) == 0) {
            return Hit{*tail, 0, Format::Rar,
                       (got >= 8 && m[7] == 1) ? "mp4+rar5" : "mp4+rar4"};
        }
        // 原子终点无魔数：干净 MP4（mdat size=0 的尾藏 zip 仍可被 ② 兜住）
    }

    // ② EOCD 反扫：zip 的精确定位（区间优先于首见魔数——诱饵本地头会偏移基址；
    //    尾部伪装数据被 length 排除，交给引擎的窗口即干净 zip）
    if (auto ext = scan_eocd(f, f.size))
        return Hit{ext->base, ext->len, Format::Zip,
                   mp4Zip ? "mp4+eocd" : "eocd"};
    if (mp4Zip)
        return Hit{mp4Off, 0, Format::Zip, mp4Desc};   // 无 EOCD 区间（zip64/损坏）：整文件直开
    return std::nullopt;
}

} // namespace nx::stego
