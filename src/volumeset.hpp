// volumeset.hpp：分片分组与完整性预检（设计 §3.3）
// 同时服务两个层级：文件系统级（目录扫描）与条目级（归档内条目名），共用同一套规则。
#pragma once
#include "util.hpp"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace nx {

struct VolumeMember {
    std::string name;      // 完整名（含序号后缀）
    uint64_t size;         // UINT64_MAX = 未知
};

struct VolumeSet {
    std::string key;              // 组键，如 "data.7z" / "backup.zip"
    std::string canonicalName;    // 拼接/合并后逻辑名（zspan/native 为 key+".zip"/key+".rar"）
    std::vector<VolumeMember> ordered;   // 按卷序
    bool zipSpan = false;         // .zNN + .zip 体系
    bool nativeRar = false;       // RAR 原生卷（不拼接，7z.dll 卷回调解析）
};

// 单名匹配（不分组）：
//  "x.7z.001"     → key "x.7z"，index 1，style=numbered（字节拼接型）
//  "x.tar.gz.013" → key "x.tar.gz"，index 13
//  "x.z01"        → key "x"，index 1，style=zspan
//  "x.part2.rar"  → key "x"，index 2，native（RAR 新式分卷）
//  "x.r00"        → key "x"，index 2（=r00 是第 2 卷），nativeOld（RAR 旧式）
//  "x.zip"/"x.rar" → 无匹配（终卷/首卷由兄弟存在性判定）
struct NameMatch {
    std::string key;
    unsigned index;
    bool zspan = false;
    bool nativeRar = false;   // 新式 .partN.rar
    bool nativeOldRar = false;// 旧式 .rNN（首卷 x.rar 不经此匹配）
};
std::optional<NameMatch> match_split_name(const std::string& name);

// 从名字集合构建分片组（仅返回完整组；不做缺失报错——由 validate 负责）
// names：本目录/本归档全部可见名字
std::map<std::string, VolumeSet> group_volumes(const std::map<std::string, uint64_t>& names);

// 完整性预检：序号连续无缺口；除最后一卷外等长（不等长仅告警）
// 返回错误描述（空 = 通过）与告警列表
void validate_set(const VolumeSet& s, std::string* errOut, std::vector<std::string>* warnsOut);

// 文件系统级接入：对输入文件做兄弟分组；input 本身必须是某组的成员
// 返回 nullopt = 不是分片（单文件路径）
std::optional<VolumeSet> group_filesystem(const std::wstring& inputPath, std::string* errOut);

} // namespace nx
