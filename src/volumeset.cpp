#include "volumeset.hpp"
#include <cctype>
#include <filesystem>
#include <set>
#include <sstream>

namespace nx {

namespace {

bool all_digits(const std::string& s) {
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return !s.empty();
}

// 剥最后一个扩展名；返回 false = 没有扩展名
bool split_ext(const std::string& name, std::string* base, std::string* ext) {
    size_t pos = name.find_last_of('.');
    if (pos == std::string::npos || pos == 0) return false;
    *base = name.substr(0, pos);
    *ext = name.substr(pos + 1);
    return true;
}

} // namespace

std::optional<NameMatch> match_split_name(const std::string& name) {
    std::string base, ext;
    if (!split_ext(name, &base, &ext)) return std::nullopt;
    // 三位以上纯数字：.001 … .999999（上限防止 stoul 溢出）
    if (ext.size() >= 3 && ext.size() <= 6 && all_digits(ext)) {
        unsigned idx = 0;
        for (char c : ext) idx = idx * 10 + static_cast<unsigned>(c - '0');
        if (idx >= 1) return NameMatch{base, idx, false};
        return std::nullopt;
    }
    // .zNN：PKZIP spanning 非终卷（z01…z9999）
    if (ext.size() >= 2 && ext.size() <= 5 && ext[0] == 'z' && all_digits(ext.substr(1))) {
        unsigned idx = 0;
        for (char c : ext.substr(1)) idx = idx * 10 + static_cast<unsigned>(c - '0');
        if (idx >= 1) return NameMatch{base, idx, true};
    }
    std::string elow = ascii_lower(ext);
    // .partN.rar：RAR 新式分卷（part1 为首卷）
    if (elow == "rar") {
        std::string pbase, pext;
        if (split_ext(base, &pbase, &pext)) {
            std::string plow = ascii_lower(pext);
            if (plow.size() >= 5 && plow.compare(0, 4, "part") == 0 && all_digits(plow.substr(4))) {
                unsigned idx = 0;
                for (char c : plow.substr(4)) idx = idx * 10 + static_cast<unsigned>(c - '0');
                if (idx >= 1 && idx <= 9999) {
                    NameMatch m;
                    m.key = pbase;
                    m.index = idx;
                    m.nativeRar = true;
                    return m;
                }
            }
        }
        return std::nullopt;   // 普通 .rar（旧式首卷由兄弟存在性判定）
    }
    // .rNN：RAR 旧式非首卷（x.r00 是第 2 卷，首卷为 x.rar）
    if (elow.size() >= 2 && elow.size() <= 4 && elow[0] == 'r' && all_digits(elow.substr(1))) {
        unsigned idx = 0;
        for (char c : elow.substr(1)) idx = idx * 10 + static_cast<unsigned>(c - '0');
        NameMatch m;
        m.key = base;
        m.index = idx + 2;      // r00 → 卷 2
        m.nativeOldRar = true;
        return m;
    }
    return std::nullopt;
}

std::map<std::string, VolumeSet> group_volumes(const std::map<std::string, uint64_t>& names) {
    // 第一遍：登记所有 numbered 成员与 zspan 成员
    struct Agg {
        std::map<unsigned, VolumeMember> byIndex;
        bool zspan = false;
    };
    std::map<std::string, Agg> aggs;
    std::set<std::string> plainNames;   // 无序号后缀的名字（候选终卷/干扰项）

    for (auto& [name, size] : names) {
        auto m = match_split_name(name);
        if (m && !m->zspan) {
            aggs[m->key].byIndex[m->index] = VolumeMember{name, size};
        } else if (m && m->zspan) {
            aggs[m->key].zspan = true;
            aggs[m->key].byIndex[m->index] = VolumeMember{name, size};
        } else {
            plainNames.insert(name);
        }
    }

    std::map<std::string, VolumeSet> out;
    for (auto& [key, agg] : aggs) {
        VolumeSet s;
        s.key = key;
        s.zipSpan = agg.zspan;
        s.canonicalName = agg.zspan ? key + ".zip" : key;   // zspan 合并后逻辑名 = x.zip
        if (!agg.zspan) {
            // 字节拼接型：按序号排序
            for (auto& [idx, mem] : agg.byIndex) {
                (void)idx;
                s.ordered.push_back(mem);
            }
        } else {
            // .zNN + .zip：终卷是 <key>.zip（含中央目录），顺序陷阱（§3.3）
            std::string terminal = key + ".zip";
            if (plainNames.count(terminal)) {
                for (auto& [idx, mem] : agg.byIndex) s.ordered.push_back(mem);
                s.ordered.push_back(VolumeMember{terminal, names.at(terminal)});
            } else {
                continue;   // 缺终卷：不成组（validate 阶段由调用方对显式成员报缺）
            }
        }
        out[key] = std::move(s);
    }

    // ---- RAR 原生卷（§3.3 原生卷型：不拼接，7z.dll 卷回调解析）----
    std::map<std::string, std::map<unsigned, VolumeMember>> nativeParts;  // 新式 .partN.rar
    std::map<std::string, std::map<unsigned, VolumeMember>> nativeOld;    // 旧式 .rNN
    for (auto& [name, size] : names) {
        auto m = match_split_name(name);
        if (!m) continue;
        if (m->nativeRar) nativeParts[m->key][m->index] = VolumeMember{name, size};
        else if (m->nativeOldRar) nativeOld[m->key][m->index] = VolumeMember{name, size};
    }
    for (auto& [key, byIdx] : nativeParts) {
        if (byIdx.size() < 2 || !byIdx.count(1)) continue;   // 单 part1 = 普通文件
        VolumeSet s;
        s.key = key;
        s.nativeRar = true;
        s.canonicalName = key + ".rar";
        for (auto& [idx, mem] : byIdx) s.ordered.push_back(mem);
        out[key] = std::move(s);
    }
    for (auto& [key, byIdx] : nativeOld) {
        std::string first = key + ".rar";
        if (!plainNames.count(first)) continue;   // 无首卷不成组
        VolumeSet s;
        s.key = key;
        s.nativeRar = true;
        s.canonicalName = key + ".rar";
        s.ordered.push_back(VolumeMember{first, names.at(first)});
        for (auto& [idx, mem] : byIdx) s.ordered.push_back(mem);   // idx 从 2 开始
        out[key] = std::move(s);
    }
    return out;
}

void validate_set(const VolumeSet& s, std::string* errOut, std::vector<std::string>* warnsOut) {
    if (errOut) errOut->clear();
    // 收集各卷序号
    std::vector<unsigned> idx;
    for (auto& mem : s.ordered) {
        auto m = match_split_name(mem.name);
        if (s.nativeRar) {
            if (m && (m->nativeRar || m->nativeOldRar)) { idx.push_back(m->index); continue; }
            std::string b, e;   // 旧式首卷 .rar → 卷 1
            if (split_ext(mem.name, &b, &e)) {
                std::string el2 = ascii_lower(e);
                if (el2 == "rar") { idx.push_back(1); continue; }
            }
            idx.push_back(0);
            continue;
        }
        std::string base, ext;
        if (!split_ext(mem.name, &base, &ext)) { idx.push_back(0); continue; }
        if (s.zipSpan) {
            if (ext == "zip") { idx.push_back(UINT32_MAX); continue; }   // 终卷
            if (ext.size() >= 2 && ext[0] == 'z' && all_digits(ext.substr(1)))
                idx.push_back(static_cast<unsigned>(std::stoul(ext.substr(1))));
            else idx.push_back(0);
        } else {
            if (all_digits(ext)) idx.push_back(static_cast<unsigned>(std::stoul(ext)));
            else idx.push_back(0);
        }
    }
    // 序号连续无缺口
    std::vector<unsigned> missing;
    if (s.nativeRar) {
        std::set<unsigned> have;
        for (unsigned v : idx) have.insert(v);
        if (have.empty() || have.count(0)) {
            if (errOut) *errOut = "分卷 [" + s.key + "] 卷序号无法解析";
            return;
        }
        unsigned start = have.count(1) ? 1 : 2;   // 旧式无 part1（首卷 .rar=1 已计入）
        unsigned n = *have.rbegin();
        for (unsigned want = start; want <= n; ++want)
            if (!have.count(want)) missing.push_back(want);
        if (!missing.empty() && errOut) {
            std::ostringstream os;
            os << "分卷 [" << s.key << "] 缺失卷号:";
            for (unsigned v : missing) os << " " << v;
            *errOut = os.str();
        }
        return;   // 原生卷无"等长"预检（卷大小由格式自定）
    }
    if (!s.zipSpan) {
        std::set<unsigned> have;
        for (unsigned v : idx) have.insert(v);
        for (unsigned want = 1; want <= idx.size(); ++want)
            if (!have.count(want)) missing.push_back(want);
    } else {
        std::set<unsigned> have;
        for (unsigned v : idx) if (v != UINT32_MAX) have.insert(v);
        unsigned n = have.empty() ? 0 : *have.rbegin();
        for (unsigned want = 1; want <= n; ++want)
            if (!have.count(want)) missing.push_back(want);
    }
    if (!missing.empty() && errOut) {
        std::ostringstream os;
        os << "分片 [" << s.key << "] 缺失卷号:";
        for (unsigned v : missing) os << " " << v;
        *errOut = os.str();
    }
    // 除最后一卷外等长（仅告警；条目大小未知时跳过）
    if (warnsOut && s.ordered.size() >= 3) {
        for (size_t i = 1; i + 1 < s.ordered.size(); ++i) {
            uint64_t a = s.ordered[0].size, b = s.ordered[i].size;
            if (a != UINT64_MAX && b != UINT64_MAX && a != b) {
                std::ostringstream os;
                os << "分片 [" << s.key << "] 非末卷长度不一致（卷 1 为 " << a
                   << "，卷 " << (i + 1) << " 为 " << b << "）";
                warnsOut->push_back(os.str());
            }
        }
    }
}

std::optional<VolumeSet> group_filesystem(const std::wstring& inputPath, std::string* errOut) {
    namespace fs = std::filesystem;
    fs::path p(inputPath);
    std::string fname = wide_to_utf8(p.filename().wstring());
    auto self = match_split_name(fname);
    // 输入是 .zNN/.NNN/.partN.rar → 找组；输入是 x.zip 且兄弟存在 x.z01.. → 终卷组；
    // 输入是 x.rar 且兄弟存在 x.r00.. → RAR 旧式首卷组
    bool wantSpanTerminal = false;
    bool wantRarFirst = false;
    std::string key;
    if (self) {
        key = self->key;
    } else {
        std::string base, ext;
        if (!split_ext(fname, &base, &ext)) return std::nullopt;
        std::string elow = ascii_lower(ext);
        if (elow == "zip") {
            key = base;
            wantSpanTerminal = true;
        } else if (elow == "rar") {
            key = base;
            wantRarFirst = true;   // 是否旧式首卷：看兄弟 .rNN 是否存在
        } else {
            return std::nullopt;
        }
    }
    std::map<std::string, uint64_t> names;
    std::error_code ec;
    fs::path dir = p.parent_path();
    if (dir.empty()) dir = L".";
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string n = wide_to_utf8(it->path().filename().wstring());
        // 只登记本组相关名字，避免误组其他文件
        if (n == key || n.rfind(key + ".", 0) == 0) names[n] = it->file_size(ec);
    }
    auto groups = group_volumes(names);
    return select_group(fname, key, self, names, groups, wantSpanTerminal, wantRarFirst, errOut);
}

// 纯（批次 3 拆分）：决策半部——组命中/单卷退化/终卷首卷意图/成员校验/预检
std::optional<VolumeSet> select_group(const std::string& fname, const std::string& key,
                                      const std::optional<NameMatch>& self,
                                      const std::map<std::string, uint64_t>& names,
                                      const std::map<std::string, VolumeSet>& groups,
                                      bool wantSpanTerminal, bool wantRarFirst,
                                      std::string* errOut) {
    auto found = groups.find(key);
    if (found == groups.end()) {
        if (wantSpanTerminal) return std::nullopt;   // 普通 zip，非分片
        if (wantRarFirst) return std::nullopt;       // 普通 rar（无 .rNN 兄弟）
        // 单卷 .001：也算一个（单成员）组 —— concat 语义退化为原文件
        if (self && !self->zspan && !self->nativeRar && !self->nativeOldRar) {
            VolumeSet s;
            s.key = key; s.canonicalName = key;
            s.ordered.push_back(VolumeMember{fname, names.count(fname) ? names.at(fname) : UINT64_MAX});
            return s;
        }
        if (errOut) *errOut = "分片 [" + key + "] 缺少终卷 " + key + ".zip";
        return std::nullopt;
    }
    // 输入必须是该组成员
    bool isMember = false;
    for (auto& m : found->second.ordered)
        if (m.name == fname) { isMember = true; break; }
    if (!isMember) {
        // 旧式首卷 x.rar 是成员（group_volumes 由 plainNames 补入）
        if (!(wantRarFirst && found->second.nativeRar)) return std::nullopt;
    }
    std::string err;
    validate_set(found->second, &err, nullptr);
    if (!err.empty()) {
        if (errOut) *errOut = err;
        return std::nullopt;
    }
    return found->second;
}

} // namespace nx
