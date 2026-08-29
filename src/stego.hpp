// stego.hpp：根文件隐写压缩包检测（README 待办 #1：MP4 尾部 / 文件尾拼接归档）
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
#include <cstdint>
#include <optional>
#include <string>

namespace nx::stego {

struct Hit {
    uint64_t offset;   // 归档起始偏移（zip = EOCD 反推基址；7z/rar = MP4 原子终点）
    uint64_t length;   // 归档长度（0 = 到 EOF；zip = EOCD 精确收尾，排除尾部伪装）
    Format fmt;        // Zip / SevenZip / Rar
    std::string desc;  // 诊断（"eocd" / "mp4+zip" …）
};

std::optional<Hit> scan(const std::wstring& path);

} // namespace nx::stego
