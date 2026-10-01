// namecodec.hpp：归档条目名码表修复（设计 §3.2：EFS 位 UTF-8 vs CP437；批次 4 拆分）
#pragma once
#include <string>

namespace nx {

// NameCodec（领域 #5）：每读取器一份——同容器内码表粘性一致，跨容器各自判定
// （原进程级全局 g_nameCp 会让多输入串包会话沿用首包语言，后续包判错）。
struct NameCodec {
    int stickyCp = 0;   // 本容器已确认的码表（0 = 未定）
};

bool strict_decode(const std::string& raw, int cp, std::wstring* out);
int score_w(const std::wstring& w);
std::string fix_name(NameCodec& codec, const char* nm);

} // namespace nx
