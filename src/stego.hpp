// stego.hpp：根文件隐写压缩包检测（设计 §5 D9：MP4 尾部 / 文件尾拼接归档）
// 仅根文件系统层（需 seek 跳过 GB 级 mdat，流式 detect 做不到）。
// 两条路：
//   ① MP4 atom 步进——逐原子头小读、按 size 跳越，走到非法头即隐写候选起点
//     （7z/rar 只能经此发现：尾部无结束标记）；mdat size=0 视为延伸到 EOF。
//   ② EOCD 反向扫描——文件尾窗口回扫 "PK\x05\x06"；不要求精确到 EOF
//     （真实样本在 EOCD 后拖伪装数据+末尾假 mdat 原子，专防尾部回扫类检测），
//     以 CD 字段数学自证：base = eocd - cdSize - cdOffset，len = EOCD 末尾 - base。
// zip 命中时 ② 的区间优先于 ① 的首见魔数位置（诱饵本地头会造成基址偏移）。
#pragma once
#include "format.hpp"
#include "util.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace nx::stego {

struct Hit {
    uint64_t offset;   // 归档起始偏移（zip = EOCD 反推基址；7z/rar = MP4 原子终点）
    uint64_t length;   // 归档长度（0 = 到 EOF；zip = EOCD 精确收尾，排除尾部伪装）
    Format fmt;        // Zip / SevenZip / Rar
    std::string desc;  // 诊断（"eocd" / "mp4+zip" …）
};

// ---- 纯核心（纯化）----

inline constexpr uint32_t be32(const byte* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
// EOCD 各字段是小端（zip 结构约定）；MP4 atom 是大端——两个读法都常驻
inline constexpr uint32_t le32(const byte* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline constexpr uint64_t be64(const byte* p) {
    return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4);
}

// MP4 原子头解释（8B：be32 size + 4B 类型）。size=1 → 64 位扩展长度（ext8 提供）；
// size=0 = 延伸到 EOF。头部与原子大小由结构携带，合法性（size ≥ 头部）由调用方判
struct AtomExtent {
    uint64_t hdrSize = 8;   // 头部字节数（含扩展长度字段）
    uint64_t atomSize = 0;  // 原子总大小（含头部）
    bool toEof = false;     // size=0：该原子延伸到 EOF（= 无尾部）
};
inline constexpr AtomExtent parse_atom_header(const byte* hdr8, const byte* ext8 = nullptr) {
    uint64_t sz = be32(hdr8);
    AtomExtent e;
    if (sz == 0) {
        e.toEof = true;
        return e;
    }
    if (sz == 1) {
        e.hdrSize = 16;
        e.atomSize = ext8 ? be64(ext8) : 1;   // ext 缺失时保持非法（< hdrSize），调用方处理
        return e;
    }
    e.atomSize = sz;
    return e;
}

// EOCD 反扫（窗口内从后往前，最近的优先）。数学：base = eocdAbs - cdOffset -
// cdSize，len = eocdEnd - base。Candidate 携带 CD 自证读取位置；Distrust =
// 数学不可信（zip64 标记 / 影子值 / 空目录），调用方应放弃回退魔数锚点
struct EocdCandidate {
    uint64_t base;          // 反推的 zip 基址
    uint64_t len;           // [base, eocdEnd) 精确窗口
    uint64_t cdVerifyPos;   // CD 末条目起点（eocdAbs - cdSize；自证 PK\x01\x02 用）
};
struct EocdScanResult {
    enum class Kind { None, Candidate, Distrust } kind = Kind::None;
    EocdCandidate cand;
};
EocdScanResult eocd_from_window(std::span<const byte> win, uint64_t winStart,
                                uint64_t fileSize);

// ---- 壳（文件 IO + 自证读取）----
std::optional<Hit> scan(const std::wstring& path);

} // namespace nx::stego
