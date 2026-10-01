// namecodec.cpp：码表探测与评分实现（批次 4 自 engines.cpp 拆分）
#include "namecodec.hpp"
#include "util.hpp"
#include <windows.h>
#include <climits>

namespace nx {

bool strict_decode(const std::string& raw, int cp, std::wstring* out) {
    int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.data(),
                                static_cast<int>(raw.size()), nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()),
                        w.data(), n);
    if (out) *out = std::move(w);
    return true;
}

int score_w(const std::wstring& w) {
    int kana = 0, cjk = 0, halfKana = 0, weird = 0;
    for (wchar_t c : w) {
        if (c >= 0x3040 && c <= 0x30FF)
            kana++;   // 全角平/片假名：日文强特征
        else if (c >= 0xFF66 && c <= 0xFF9D)
            halfKana++;   // 半角片假名：GBK 流被误按 932 解的典型征兆
        else if (c >= 0x4E00 && c <= 0x9FFF)
            cjk++;
        else if (c < 0x20)
            weird++;
    }
    return kana * 4 + cjk - halfKana * 2 - weird * 8;
}

std::string fix_name(NameCodec& codec, const char* nm) {
    if (!nm) return "";
    std::string s(nm);
    bool high = false;
    for (unsigned char c : s)
        if (c >= 0x80) { high = true; break; }
    if (!high) return s;

    // 原始字节候选：s 本身非法 UTF-8（libarchive 漏转）或反推 CP437（乱码转换可逆）
    std::string raw = s;
    std::wstring cur;
    if (strict_decode(s, CP_UTF8, &cur)) {
        // s 是合法 UTF-8：真 EFS 名（含 CP437 外字符 → 保留）或 CP437 乱码（可逆）
        char buf[4096];
        BOOL usedDef = FALSE;
        int n = WideCharToMultiByte(437, 0, cur.data(), static_cast<int>(cur.size()), buf,
                                    sizeof(buf), nullptr, &usedDef);
        if (usedDef || n <= 0)
            return s;   // 含 CP437 外字符 → 真实 Unicode 名
        // 长名截断保护（4096 内不适用）
        raw.assign(buf, static_cast<size_t>(n));
        // 反推后再试严格 UTF-8：无 EFS 但实为 UTF-8 的写入器
        std::wstring retry;
        if (strict_decode(raw, CP_UTF8, &retry)) {
            bool retryHigh = false;
            for (wchar_t c : retry)
                if (c >= 0x80) { retryHigh = true; break; }
            if (retryHigh)
                return wide_to_utf8(retry);
            return s;   // 反推后全 ASCII：原名即 ASCII 安全
        }
    }

    // CJK 码表选择：粘性优先，否则评分（候选顺序 = 平手优先级）
    int acp = GetACP();
    int cand[5] = {acp, 936, 950, 949, 932};
    if (codec.stickyCp) {
        std::wstring w;
        if (strict_decode(raw, codec.stickyCp, &w))
            return wide_to_utf8(w);
    }
    int bestCp = 0, bestScore = INT_MIN;
    std::wstring bestW;
    for (int cp : cand) {
        std::wstring w;
        if (!strict_decode(raw, cp, &w)) continue;
        int sc = score_w(w);
        if (sc > bestScore) {
            bestScore = sc;
            bestCp = cp;
            bestW = std::move(w);
        }
    }
    if (bestCp) {
        codec.stickyCp = bestCp;
        return wide_to_utf8(bestW);
    }
    return s;   // 全部失败：保留 libarchive 原名（不再产生空名）
}

} // namespace nx
