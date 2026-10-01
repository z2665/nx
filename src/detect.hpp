// detect.hpp：内容嗅探（设计 D1：magic 表 + 轻量结构校验，扩展名仅辅助）
#pragma once
#include "bytesource.hpp"
#include "format.hpp"
#include <optional>
#include <string>

namespace nx {

struct Detection {
    Format fmt = Format::Unknown;
    std::string detail;                 // 展示性附注（"rar5" / "stored"…）
    std::optional<uint64_t> sfxOffset;  // SFX 前缀内偏移（领域 #12：原序列化成
                                        // 字符串进 detail 后即丢弃，无法结构化使用）
    // 完整展示名："zip deflate/其他" / "zip SFX@+1234" / "rar rar5 SFX@+56"
    std::string display() const {
        std::string s = format_name(fmt);
        if (!detail.empty()) s += " " + detail;
        if (sfxOffset) s += " SFX@+" + std::to_string(*sfxOffset);
        return s;
    }
};

// 纯核心（P5）：对给定字节窗口嗅探——不碰流、不补拉；SFX 扫描覆盖到窗口末尾。
// 壳层负责决定窗口大小（64KiB 首扫 → 未命中补拉 4MiB）
Detection detect_from_bytes(std::span<const byte> p, std::optional<uint64_t> sizeHint);

// 在 PushbackSource 上嗅探（不消费）。nameHint 仅作辅助（M0 未用扩展名判格式）。
Detection detect(PushbackSource& src, const std::string& nameHint);

} // namespace nx
