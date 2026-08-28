// detect.hpp：内容嗅探（设计 D1：magic 表 + 轻量结构校验，扩展名仅辅助）
#pragma once
#include "bytesource.hpp"
#include "format.hpp"
#include <string>

namespace nx {

struct Detection {
    Format fmt = Format::Unknown;
    std::string detail;   // 附加信息（如 "tar(ustar)" / 校验失败原因）
};

// 在 PushbackSource 上嗅探（不消费）。nameHint 仅作辅助（M0 未用扩展名判格式）。
Detection detect(PushbackSource& src, const std::string& nameHint);

} // namespace nx
