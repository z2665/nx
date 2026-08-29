// stego.hpp：根文件隐写压缩包检测（README 待办 #1：MP4 尾部 / 文件尾拼接归档）
// 仅根文件系统层（需 seek 跳过 GB 级 mdat，流式 detect 做不到）。
// 两条路：
//   ① MP4 atom 步进——逐原子头小读、按 size 跳越，走到非法头即隐写候选起点
//     （7z/rar 只能经此发现：尾部无结束标记）；mdat size=0 视为延伸到 EOF。
//   ② EOCD 反向扫描——zip 的 EOCD 天然在文件末（≤64KiB 注释），从尾回扫
//     "PK\x05\x06" 且 commentLen 精确吃到 EOF；覆盖任意格式文件的尾接 zip。
#pragma once
#include "format.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace nx::stego {

struct Hit {
    uint64_t offset;   // 归档起始偏移（zip 经 EOCD 反推，zip64 时为 0 交由引擎自定位）
    Format fmt;        // Zip / SevenZip / Rar
    std::string desc;  // 诊断（"mp4+zip" / "eocd" …）
};

std::optional<Hit> scan(const std::wstring& path);

} // namespace nx::stego
