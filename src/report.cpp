// report.cpp：--report JSON 渲染（纯函数；原 main.cpp 内静态实现，不可单测）
#include "report.hpp"

namespace nx {

namespace {

std::string json_escape(const std::string& v) {
    std::string r;
    r.reserve(v.size() + 2);
    r += '"';
    for (unsigned char c : v) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    r += buf;
                } else {
                    r += static_cast<char>(c);
                }
        }
    }
    r += '"';
    return r;
}

} // namespace

std::string render_report(const ReportData& d) {
    std::string j;
    j += "{\n  \"tool\": " + json_escape(d.tool) + ",\n";
    j += "  \"inputs\": [";
    for (size_t i = 0; i < d.inputs.size(); ++i) {
        if (i) j += ", ";
        j += json_escape(d.inputs[i]);
    }
    j += "],\n";
    j += "  \"files\": " + std::to_string(d.files) + ",\n";
    j += "  \"bytes\": " + std::to_string(d.bytes) + ",\n";
    j += "  \"inputBytes\": " + std::to_string(d.inputBytes) + ",\n";
    j += "  \"expansionRatio\": " + std::to_string(d.expansionRatio) + ",\n";
    j += "  \"containers\": " + std::to_string(d.containers) + ",\n";
    j += "  \"filters\": " + std::to_string(d.filters) + ",\n";
    j += "  \"durationMs\": " + std::to_string(d.durationMs) + ",\n";
    j += "  \"corruptEntries\": " + std::to_string(d.corruptEntries) + ",\n";
    j += "  \"failedBranches\": " + std::to_string(d.failedBranches) + ",\n";
    j += "  \"passwordPrompts\": " + std::to_string(d.passwordPrompts) + ",\n";
    j += "  \"verify\": ";
    if (d.verifyEnabled) {
        j += "[\n";
        auto& files = d.verified;
        for (size_t i = 0; i < files.size(); ++i) {
            j += "    {\"path\": " + json_escape(files[i].rel) +
                 ", \"bytes\": " + std::to_string(files[i].bytes) +
                 ", \"sha256\": \"" + files[i].sha256 + "\"}";
            j += (i + 1 < files.size()) ? ",\n" : "\n";
        }
        j += "  ]\n";
    } else {
        j += "null\n";
    }
    j += "}\n";
    return j;
}

} // namespace nx
