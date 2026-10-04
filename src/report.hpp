// report.hpp：--report JSON 组装（纯渲染：snapshot + render 分离）
// gather（Stats/Sink 快照）在 main；此处只有值 → 文本，可单测。
// 纪律（设计 D8）：不含任何密码信息。
#pragma once
#include "sink.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace nx {

struct ReportData {
    std::string tool;
    std::vector<std::string> inputs;   // UTF-8
    uint64_t files = 0;
    uint64_t bytes = 0;
    uint64_t inputBytes = 0;
    double expansionRatio = 0;
    uint64_t containers = 0;
    uint64_t filters = 0;
    uint64_t durationMs = 0;
    int corruptEntries = 0;
    int failedBranches = 0;
    uint64_t passwordPrompts = 0;
    bool verifyEnabled = false;
    std::vector<VerifiedFile> verified;
};

// 纯：ReportData → JSON 文本（与原 build_report 输出逐字段一致）
std::string render_report(const ReportData& d);

} // namespace nx
