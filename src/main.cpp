// main.cpp：CLI 入口（设计 §8）
// nx extract <输入...> -O <目录> [选项]
// nx tree   <输入...> [选项]
// 退出码：0 成功 / 1 部分失败 / 2 密码缺失或耗尽 / 3 超限熔断 / 4 缺分片 / 64 用法错误
#include "walker.hpp"
#include "password.hpp"
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace nx;

namespace {

void usage() {
    std::printf(
        "nx - 流式嵌套压缩包解压工具 (M0)\n"
        "\n"
        "用法:\n"
        "  nx extract <输入...> -O <输出目录> [选项]\n"
        "  nx tree <输入...> [选项]\n"
        "\n"
        "选项:\n"
        "  -O <目录>            输出目录（extract 必填）\n"
        "  -p <密码>            候选密码（可重复，按序试探）\n"
        "  --password-file <f>  密码文件（每行一个）\n"
        "  --no-prompt          非交互：不询问，仅候选列表\n"
        "  --depth <n>          递归深度上限（默认 8）\n"
        "  --max-bytes <n>      累计输出上限（默认 512G；支持 K/M/G/T）\n"
        "  --max-ratio <n>      压缩比熔断（默认 1000）\n"
        "  --keep-going         数据损坏时隔离该条目并继续\n"
        "  --spool-ram <n>      随机访问容器 RAM 驻留上限（默认 64M）\n"
        "  --buffer <n>         级间缓冲（默认 1M）\n"
        "  --temp-dir <目录>    溢出临时目录（默认系统临时目录）\n"
        "\n"
        "退出码: 0 成功 | 1 部分失败 | 2 密码缺失或耗尽 | 3 超限熔断 | 4 缺分片 | 64 用法错误\n");
}

std::vector<std::string> get_args(int& argc) {
    // 宽字符参数 → UTF-8（控制台代码页无关）
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> out;
    if (wargv) {
        for (int i = 0; i < argc; ++i) out.push_back(wide_to_utf8(wargv[i]));
        LocalFree(wargv);
    }
    return out;
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    int argc = 0;
    auto args = get_args(argc);
    if (argc < 2) { usage(); return 64; }
    std::string cmd = args[1];
    if (cmd == "--help" || cmd == "-h" || cmd == "help") { usage(); return 0; }
    if (cmd != "extract" && cmd != "tree") {
        std::fprintf(stderr, "[nx] 未知命令: %s\n", cmd.c_str());
        usage();
        return 64;
    }
    bool dryRun = (cmd == "tree");

    Session s;
    std::vector<std::wstring> inputs;
    std::wstring outDir;
    bool haveOut = false;

    try {
        for (int i = 2; i < argc; ++i) {
            std::string a = args[i];
            auto need = [&](const char* what) -> std::string {
                if (i + 1 >= argc) {
                    std::fprintf(stderr, "[nx] %s 缺少参数\n", what);
                    std::exit(64);
                }
                return args[++i];
            };
            if (a == "-O" || a == "--out") { outDir = utf8_to_wide(need("-O")); haveOut = true; }
            else if (a == "-p" || a == "--password") s.pw.addCandidate(need("-p"));
            else if (a == "--password-file") s.pw.loadPasswordFile(utf8_to_wide(need("--password-file")));
            else if (a == "--no-prompt") s.pw.setNoPrompt(true);
            else if (a == "--depth") s.opt.maxDepth = std::stoi(need("--depth"));
            else if (a == "--max-bytes") s.opt.maxBytes = parse_size(need("--max-bytes"));
            else if (a == "--max-ratio") s.opt.maxRatio = std::stoull(need("--max-ratio"));
            else if (a == "--keep-going") s.opt.keepGoing = true;
            else if (a == "--spool-ram") s.opt.spoolRam = static_cast<size_t>(parse_size(need("--spool-ram")));
            else if (a == "--buffer") s.opt.pipeBytes = static_cast<size_t>(parse_size(need("--buffer")));
            else if (a == "--temp-dir") s.tempDir = utf8_to_wide(need("--temp-dir"));
            else if (!a.empty() && a[0] == '-') {
                std::fprintf(stderr, "[nx] 未知选项: %s\n", a.c_str());
                return 64;
            } else {
                inputs.push_back(utf8_to_wide(a));
            }
        }
    } catch (const Error& e) {
        std::fprintf(stderr, "[nx] %s\n", e.what());
        return 64;
    }

    if (inputs.empty()) {
        std::fprintf(stderr, "[nx] 未指定输入文件\n");
        return 64;
    }
    if (!dryRun && !haveOut) {
        std::fprintf(stderr, "[nx] extract 需要 -O <输出目录>\n");
        return 64;
    }
    if (dryRun) { s.opt.dryRun = true; outDir = L""; }

    // 输出目录预创建 + 磁盘水位预检
    if (!dryRun) {
        if (!ensure_dir_recursive(outDir)) {
            std::fprintf(stderr, "[nx] 创建输出目录失败: %s\n", wide_to_utf8(outDir).c_str());
            return 1;
        }
    }

    s.sink = std::make_unique<Sink>(outDir, s.opt, s.stats, dryRun);

    for (auto& in : inputs) {
        try {
            run_input(s, in);
        } catch (LimitError& e) {
            std::fprintf(stderr, "[nx] ✗ 超限熔断：%s\n", e.what());
            s.stats.limitTripped = true;
        } catch (MissingVolumes& e) {
            std::fprintf(stderr, "[nx] ✗ %s\n", e.what());
            s.stats.sawMissingVol = true;
        } catch (PasswordExhausted& e) {
            std::fprintf(stderr, "[nx] ✗ %s\n", e.what());
            s.stats.sawPasswordFail = true;
        } catch (CorruptError& e) {
            std::fprintf(stderr, "[nx] ✗ %s\n", e.what());
            s.stats.sawCorrupt = true;
        } catch (Error& e) {
            std::fprintf(stderr, "[nx] ✗ %s\n", e.what());
            s.stats.branchesFailed.fetch_add(1);
        }
    }

    // 汇总（设计 D8：不含任何密码信息）
    uint64_t inB = s.meter.bytes.load();
    std::printf(
        "✓ %llu 个文件 · %s 输出 · %llu 层容器 · %llu 层过滤器",
        static_cast<unsigned long long>(s.stats.filesOut.load()),
        format_size(s.stats.bytesOut.load()).c_str(),
        static_cast<unsigned long long>(s.stats.containers.load()),
        static_cast<unsigned long long>(s.stats.filters.load()));
    if (!dryRun) {
        std::printf(" · 输入 %s", format_size(inB).c_str());
        if (inB > 0) {
            std::printf("（膨胀 %.1f×）", s.stats.bytesOut.load() / static_cast<double>(inB));
        }
    }
    std::printf("\n");
    if (s.stats.corrupt.load() || s.stats.branchesFailed.load()) {
        std::printf("  失败分支 %d · 损坏条目 %d\n", s.stats.branchesFailed.load(),
                    s.stats.corrupt.load());
    }

    // 退出码优先级：超限(3) > 密码(2) > 缺分片(4) > 部分失败(1) > 成功(0)
    if (s.stats.limitTripped.load()) return 3;
    if (s.stats.sawPasswordFail.load()) return 2;
    if (s.stats.sawMissingVol.load()) return 4;
    if (s.stats.sawCorrupt.load() || s.stats.branchesFailed.load()) return 1;
    return 0;
}
