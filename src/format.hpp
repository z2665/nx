// format.hpp：格式枚举与分类（设计 §3）
#pragma once
#include "util.hpp"
#include <string>

namespace nx {

enum class Format {
    Unknown,
    // 压缩过滤器（单流→单流）
    Gzip, Bzip2, Xz, Lzma, Zstd, Lz4, CompressZ, Brotli,
    // 容器
    Tar, Cpio, Ar,        // S：顺序容器
    Zip,                  // Z：尾部依赖容器
    SevenZip, Rar, Iso, Cab, Wim,  // R：随机访问容器
};

enum class FormatClass { None, Filter, SeqContainer, TailContainer, RandContainer };

// 格式知识表：格式 → {类属, 名称} 的唯一事实源——
// 原 classify()/format_name() 两个平行 switch 各自维护一份映射，改一处漏一处
struct FormatInfo {
    Format fmt;
    FormatClass cls;
    const char* name;
};
constexpr FormatInfo kFormatTable[] = {
    {Format::Gzip, FormatClass::Filter, "gzip"},
    {Format::Bzip2, FormatClass::Filter, "bzip2"},
    {Format::Xz, FormatClass::Filter, "xz"},
    {Format::Lzma, FormatClass::Filter, "lzma"},
    {Format::Zstd, FormatClass::Filter, "zstd"},
    {Format::Lz4, FormatClass::Filter, "lz4"},
    {Format::CompressZ, FormatClass::Filter, "compress(.Z)"},
    {Format::Brotli, FormatClass::Filter, "brotli"},
    {Format::Tar, FormatClass::SeqContainer, "tar"},
    {Format::Cpio, FormatClass::SeqContainer, "cpio"},
    {Format::Ar, FormatClass::SeqContainer, "ar"},
    {Format::Zip, FormatClass::TailContainer, "zip"},
    {Format::SevenZip, FormatClass::RandContainer, "7z"},
    {Format::Rar, FormatClass::RandContainer, "rar"},
    {Format::Iso, FormatClass::RandContainer, "iso"},
    {Format::Cab, FormatClass::RandContainer, "cab"},
    {Format::Wim, FormatClass::RandContainer, "wim"},
};

constexpr FormatInfo format_info(Format f) {
    for (const FormatInfo& e : kFormatTable)
        if (e.fmt == f) return e;
    return {Format::Unknown, FormatClass::None, "unknown"};
}

inline FormatClass classify(Format f) { return format_info(f).cls; }
inline const char* format_name(Format f) { return format_info(f).name; }

// 过滤器层后缀剥离（用于裸过滤器条目的输出命名）
inline std::string strip_filter_suffixes(const std::string& name) {
    static const char* suffixes[] = {
        ".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tar.xz", ".txz", ".tar.zst", ".tar.lz4",
        ".gz", ".bz2", ".xz", ".zst", ".lz4", ".z", ".lzma",
        // ↑ .z 须以小写登记（L4）：比较前名字已统一小写，大写条目永不匹配
    };
    std::string low = ascii_lower(name);
    for (const char* sfx : suffixes) {
        std::string sl(sfx);
        if (low.size() > sl.size() && low.compare(low.size() - sl.size(), sl.size(), sfx) == 0)
            return name.substr(0, name.size() - sl.size());
    }
    return name + "~";   // 无可剥离后缀：加后缀避免与目录重名混淆
}

} // namespace nx
