// zipcd.hpp：zip 文件名码表探测（§3.2）——无 EFS 标志 + 无
// hdrcharset 时 libarchive 对无法按 UTF-8 校验的名字返回 NULL → 下游消毒成 "_"。
// 对候选码表逐一试开（seekable 遍历中央目录，不读数据），按"零空名 + 假名加分"择优
#pragma once
#include "laimp.hpp"
#include "namecodec.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <string>

namespace nx {

template <class ViewFactory>
std::string detect_zip_charset(ViewFactory makeView) {
    static int n = 0;
    int acp = GetACP();
    std::vector<std::string> cands;
    cands.push_back("");                       // EFS/纯 ASCII：无需选项
    if (acp != 932 && acp != 936 && acp != 949 && acp != 950 && acp != 1252)
        cands.push_back("CP" + std::to_string(acp));  // 系统码表（非 CJK 默认集时）
    cands.push_back("CP936");                  // GBK
    cands.push_back("CP932");                  // Shift-JIS
    cands.push_back("CP950");                  // Big5
    cands.push_back("CP949");                  // EUC-KR

    std::string best;
    long bestScore = LONG_MIN;
    for (auto& cp : cands) {
        archive* a = archive_read_new();
        archive_read_support_format_zip(a);
        if (!cp.empty()) {
            std::string opt = "zip:hdrcharset=" + cp;
            archive_read_set_options(a, opt.c_str());
        }
        auto view = makeView();
        CbCtx ctx;
        ctx.view = view.get();
        ctx.buf.assign(256 << 10, 0);
        archive_read_set_read_callback(a, la_read_cb);
        archive_read_set_close_callback(a, [](archive*, void*) { return ARCHIVE_OK; });
        archive_read_set_seek_callback(a, la_seek_cb);
        archive_read_set_callback_data(a, &ctx);
        bool ok = archive_read_open1(a) == ARCHIVE_OK;
        long score = 0;
        int highSeen = 0;
        archive_entry* e = nullptr;
        int nullNames = 0;
        while (ok && archive_read_next_header(a, &e) == ARCHIVE_OK) {
            const char* nm = archive_entry_pathname(e);
            if (!nm) {
                ++nullNames;
                break;   // 该码表下仍有空名 → 拒绝
            }
            bool hi = false;
            for (const char* p = nm; *p; ++p)
                if ((unsigned char)*p >= 0x80) { hi = true; break; }
            if (hi) {
                std::wstring w = utf8_to_wide(nm);
                score += score_w(w);
                if (++highSeen >= 128) break;
            }
        }
        archive_read_free(a);
        if (nullNames > 0) continue;
        if (score > bestScore) {
            bestScore = score;
            best = cp;
        }
    }
    return best;
}

} // namespace nx
