// format.hpp：格式枚举与分类（设计 §3）
#pragma once
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

inline FormatClass classify(Format f) {
    switch (f) {
        case Format::Gzip: case Format::Bzip2: case Format::Xz: case Format::Lzma:
        case Format::Zstd: case Format::Lz4: case Format::CompressZ: case Format::Brotli:
            return FormatClass::Filter;
        case Format::Tar: case Format::Cpio: case Format::Ar:
            return FormatClass::SeqContainer;
        case Format::Zip:
            return FormatClass::TailContainer;
        case Format::SevenZip: case Format::Rar: case Format::Iso:
        case Format::Cab: case Format::Wim:
            return FormatClass::RandContainer;
        default:
            return FormatClass::None;
    }
}

inline const char* format_name(Format f) {
    switch (f) {
        case Format::Gzip: return "gzip";
        case Format::Bzip2: return "bzip2";
        case Format::Xz: return "xz";
        case Format::Lzma: return "lzma";
        case Format::Zstd: return "zstd";
        case Format::Lz4: return "lz4";
        case Format::CompressZ: return "compress(.Z)";
        case Format::Brotli: return "brotli";
        case Format::Tar: return "tar";
        case Format::Cpio: return "cpio";
        case Format::Ar: return "ar";
        case Format::Zip: return "zip";
        case Format::SevenZip: return "7z";
        case Format::Rar: return "rar";
        case Format::Iso: return "iso";
        case Format::Cab: return "cab";
        case Format::Wim: return "wim";
        default: return "unknown";
    }
}

// 过滤器层后缀剥离（用于裸过滤器条目的输出命名）
inline std::string strip_filter_suffixes(const std::string& name) {
    static const char* suffixes[] = {
        ".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tar.xz", ".txz", ".tar.zst", ".tar.lz4",
        ".gz", ".bz2", ".xz", ".zst", ".lz4", ".z", ".lzma",
        // ↑ .Z 须以小写登记（L4）：比较前名字已统一小写，大写条目永不匹配
    };
    std::string low = name;
    for (auto& c : low) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (const char* sfx : suffixes) {
        std::string sl(sfx);
        if (low.size() > sl.size() && low.compare(low.size() - sl.size(), sl.size(), sfx) == 0)
            return name.substr(0, name.size() - sl.size());
    }
    return name + "~";   // 无可剥离后缀：加后缀避免与目录重名混淆
}

} // namespace nx
