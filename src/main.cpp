// main.cpp：CLI 入口（设计 §8 + M3：右键菜单动词、GUI 密码/前缀、默认日志）
// 退出码：0 成功 / 1 部分失败 / 2 密码缺失或耗尽（含用户取消）/ 3 超限熔断 / 4 缺分片 / 64 用法错误
//
// 双模式 exe（/SUBSYSTEM:WINDOWS）：资源管理器右键启动不闪黑框；
// 从终端/管道启动时继承句柄，控制台与管道输出行为与常规 CLI 完全一致。
#include "walker.hpp"
#include "password.hpp"
#include "menu.hpp"
#include "gui.hpp"
#include "log.hpp"
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale.h>
#include <string>
#include <vector>

using namespace nx;

namespace {

void usage() {
    log_out(
        "nx - 流式嵌套压缩包解压工具 (M3)\n"
        "\n"
        "用法:\n"
        "  nx extract <输入...> -O <输出目录> [选项]\n"
        "  nx extract-here <输入...>          # 解压到各输入所在目录（不建根目录层）\n"
        "  nx extract-into <输入>             # GUI 询问前缀目录（默认=压缩文件名）\n"
        "  nx tree <输入...> [选项]\n"
        "  nx menu install | remove           # 资源管理器右键菜单（当前用户，免管理员）\n"
        "\n"
        "选项:\n"
        "  -O <目录>            输出目录（extract 必填）\n"
        "  -p <密码>            候选密码（可重复，按序试探）\n"
        "  --password-file <f>  密码文件（每行一个）\n"
        "  --no-prompt          非交互：不询问，仅候选列表\n"
        "  --gui                密码经 GUI 弹窗（无控制台时自动启用）\n"
        "  --no-root            不建根目录层（条目直接落在输出目录）\n"
        "  --depth <n>          递归深度上限（默认 8）\n"
        "  --max-bytes <n>      累计输出上限（默认 512G；支持 K/M/G/T）\n"
        "  --max-ratio <n>      压缩比熔断（默认 1000）\n"
        "  --keep-going         数据损坏时隔离该条目并继续\n"
        "  --spool-ram <n>      随机访问容器 RAM 驻留上限（默认 64M）\n"
        "  --buffer <n>         级间缓冲（默认 1M）\n"
        "  --temp-dir <目录>    溢出临时目录（默认系统临时目录）\n"
        "  --verify sha256      输出文件 sha256 校验（计入 --report 与日志）\n"
        "  --report <f.json>    机器可读报告（统计/耗时/校验；不含密码，D8）\n"
        "\n"
        "日志: 每次运行默认记录到 nx.exe 所在目录 nx.log（append，超 5 MiB 截断）\n"
        "退出码: 0 成功 | 1 部分失败 | 2 密码缺失或耗尽/用户取消 | 3 超限熔断 | 4 缺分片 | 64 用法错误\n");
}

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

// --report 内容（D8）：统计/耗时/校验；不含任何密码信息。始终进日志（M3 需求 6）
std::string build_report(Session& s, const std::vector<std::wstring>& inputs,
                         ULONGLONG elapsedMs, const std::string& tool) {
    std::string j;
    j += "{\n  \"tool\": " + json_escape(tool) + ",\n";
    j += "  \"inputs\": [";
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (i) j += ", ";
        j += json_escape(wide_to_utf8(inputs[i]));
    }
    j += "],\n";
    j += "  \"files\": " + std::to_string(s.stats.filesOut.load()) + ",\n";
    j += "  \"bytes\": " + std::to_string(s.stats.bytesOut.load()) + ",\n";
    j += "  \"inputBytes\": " + std::to_string(s.meter.bytes.load()) + ",\n";
    double inB = static_cast<double>(s.meter.bytes.load());
    j += "  \"expansionRatio\": " +
         std::to_string(inB > 0 ? s.stats.bytesOut.load() / inB : 0.0) + ",\n";
    j += "  \"containers\": " + std::to_string(s.stats.containers.load()) + ",\n";
    j += "  \"filters\": " + std::to_string(s.stats.filters.load()) + ",\n";
    j += "  \"durationMs\": " + std::to_string(elapsedMs) + ",\n";
    j += "  \"corruptEntries\": " + std::to_string(s.stats.corrupt.load()) + ",\n";
    j += "  \"failedBranches\": " + std::to_string(s.stats.branchesFailed.load()) + ",\n";
    j += "  \"passwordPrompts\": " + std::to_string(s.pw.promptCount()) + ",\n";
    j += "  \"verify\": ";
    if (s.sink && s.sink->verifyEnabled()) {
        j += "[\n";
        auto& files = s.sink->verified();
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

void write_report_file(const std::wstring& path, const std::string& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log_err("[nx] 报告写入失败: %s\n", wide_to_utf8(path).c_str());
        return;
    }
    DWORD w = 0;
    WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &w, nullptr);
    CloseHandle(h);
}

std::vector<std::string> get_args(int& argc) {
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> out;
    if (wargv) {
        for (int i = 0; i < argc; ++i) out.push_back(wide_to_utf8(wargv[i]));
        LocalFree(wargv);
    }
    return out;
}

std::wstring parent_dir_of(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L".";
    if (p == 2 && path[1] == L':') return path.substr(0, 3);   // 盘根
    return path.substr(0, p);
}

bool console_attached() {
    return GetConsoleWindow() != nullptr;
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    // §3.2 关键：UTF-8 locale —— 否则 libarchive 的文件名转换整体失效
    //（C locale 下非 ASCII 名直接得到 NULL pathname → 下游消毒成 "_"）
    setlocale(LC_ALL, ".UTF8");
    int argc = 0;
    auto args = get_args(argc);
    log_open(argc, nullptr);   // 默认日志（M3 需求 6）：始终开启
    if (argc < 2) { usage(); return 64; }
    std::string cmd = args[1];
    if (cmd == "--help" || cmd == "-h" || cmd == "help") { usage(); return 0; }

    // ---- 右键菜单管理（M3 需求 2）----
    if (cmd == "menu") {
        std::string action = argc >= 3 ? args[2] : "";
        std::string err;
        if (action == "install") {
            if (menu_install(&err)) {
                log_out("[nx] 右键菜单已安装（当前用户）：nx 解压 → 解压到当前目录 / 解压到指定目录…\n");
                return 0;
            }
            log_err("[nx] 安装失败: %s\n", err.c_str());
            return 1;
        }
        if (action == "remove") {
            if (menu_remove(&err)) {
                log_out("[nx] 右键菜单已移除\n");
                return 0;
            }
            log_err("[nx] 移除失败: %s\n", err.c_str());
            return 1;
        }
        log_err("[nx] 用法: nx menu install | remove\n");
        return 64;
    }

    if (cmd != "extract" && cmd != "extract-here" && cmd != "extract-into" && cmd != "tree") {
        log_err("[nx] 未知命令: %s\n", cmd.c_str());
        usage();
        return 64;
    }
    bool dryRun = (cmd == "tree");

    Session s;
    std::vector<std::wstring> inputs;
    std::wstring outDir;
    std::wstring reportPath;
    bool haveOut = false;
    bool forceNoRoot = (cmd == "extract-here" || cmd == "extract-into");
    ULONGLONG t0 = GetTickCount64();

    try {
        for (int i = 2; i < argc; ++i) {
            std::string a = args[i];
            auto need = [&](const char* what) -> std::string {
                if (i + 1 >= argc) {
                    log_err("[nx] %s 缺少参数\n", what);
                    std::exit(64);
                }
                return args[++i];
            };
            if (a == "-O" || a == "--out") { outDir = utf8_to_wide(need("-O")); haveOut = true; }
            else if (a == "-p" || a == "--password") s.pw.addCandidate(need("-p"));
            else if (a == "--password-file") s.pw.loadPasswordFile(utf8_to_wide(need("--password-file")));
            else if (a == "--no-prompt") s.pw.setNoPrompt(true);
            else if (a == "--gui") { s.pw.setGuiPrompt(true); s.opt.guiPrompt = true; }
            else if (a == "--no-root") s.opt.noRoot = true;
            else if (a == "--depth") s.opt.maxDepth = std::stoi(need("--depth"));
            else if (a == "--max-bytes") s.opt.maxBytes = parse_size(need("--max-bytes"));
            else if (a == "--max-ratio") s.opt.maxRatio = std::stoull(need("--max-ratio"));
            else if (a == "--keep-going") s.opt.keepGoing = true;
            else if (a == "--spool-ram") s.opt.spoolRam = static_cast<size_t>(parse_size(need("--spool-ram")));
            else if (a == "--buffer") s.opt.pipeBytes = static_cast<size_t>(parse_size(need("--buffer")));
            else if (a == "--temp-dir") s.tempDir = utf8_to_wide(need("--temp-dir"));
            else if (a == "--verify") {
                std::string algo = need("--verify");
                if (algo != "sha256") {
                    log_err("[nx] --verify 仅支持 sha256\n");
                    return 64;
                }
                s.opt.verify = algo;
            }
            else if (a == "--report") reportPath = utf8_to_wide(need("--report"));
            else if (!a.empty() && a[0] == '-') {
                log_err("[nx] 未知选项: %s\n", a.c_str());
                return 64;
            } else {
                inputs.push_back(utf8_to_wide(a));
            }
        }
    } catch (const Error& e) {
        log_err("[nx] %s\n", e.what());
        return 64;
    }

    if (inputs.empty()) {
        log_err("[nx] 未指定输入文件\n");
        return 64;
    }
    if (dryRun) { s.opt.dryRun = true; outDir = L""; }
    if (forceNoRoot) s.opt.noRoot = true;

    // ---- extract-here / extract-into 的输出目录语义（M3 需求 3）----
    std::wstring prefixDir;
    if (cmd == "extract-here") {
        if (!haveOut) {
            // 每个输入解到其所在目录
            outDir = parent_dir_of(inputs[0]);
            if (inputs.size() > 1) {
                log_err("[nx] extract-here 一次只处理一个输入（右键语义）\n");
                return 64;
            }
        }
    } else if (cmd == "extract-into") {
        if (inputs.size() > 1) {
            log_err("[nx] extract-into 一次只处理一个输入（右键语义）\n");
            return 64;
        }
        // 前缀 GUI：默认填与压缩文件名一样的前缀；X/取消 → 直接退出（需求 3/5）
        std::wstring fname = inputs[0].substr(inputs[0].find_last_of(L"\\/") + 1);
        auto prefix = gui::ask_prefix(fname);
        if (!prefix || prefix->empty()) {
            log_raw("用户取消了前缀输入，退出\n");
            return 2;
        }
        outDir = parent_dir_of(inputs[0]) + L"\\" + *prefix;
        prefixDir = *prefix;
    }

    if (!dryRun && !haveOut && cmd == "extract") {
        log_err("[nx] extract 需要 -O <输出目录>\n");
        return 64;
    }
    if (!dryRun) {
        if (!ensure_dir_recursive(outDir)) {
            log_err("[nx] 创建输出目录失败: %s\n", wide_to_utf8(outDir).c_str());
            return 1;
        }
    }

    s.sink = std::make_unique<Sink>(outDir, s.opt, s.stats, dryRun);

    bool cancelled = false;
    for (auto& in : inputs) {
        try {
            run_input(s, in);
        } catch (Cancelled&) {
            cancelled = true;   // GUI 取消：静默退出（M3 需求 5）
            break;
        } catch (LimitError& e) {
            log_err("[nx] ✗ 超限熔断：%s\n", e.what());
            s.stats.limitTripped = true;
        } catch (MissingVolumes& e) {
            log_err("[nx] ✗ %s\n", e.what());
            s.stats.sawMissingVol = true;
        } catch (PasswordExhausted& e) {
            log_err("[nx] ✗ %s\n", e.what());
            s.stats.sawPasswordFail = true;
        } catch (CorruptError& e) {
            log_err("[nx] ✗ %s\n", e.what());
            s.stats.sawCorrupt = true;
        } catch (Error& e) {
            log_err("[nx] ✗ %s\n", e.what());
            s.stats.branchesFailed.fetch_add(1);
        }
    }

    s.sink->waitAll();
    ULONGLONG elapsedMs = GetTickCount64() - t0;

    // 报告：始终入日志；--report 时另存文件（M3 需求 6 + D8）
    std::string report = build_report(s, inputs, elapsedMs, argc ? args[0] : "nx");
    if (!reportPath.empty())
        write_report_file(reportPath, report);
    log_raw(report);

    // 汇总
    uint64_t inB = s.meter.bytes.load();
    log_out("✓ %llu 个文件 · %s 输出 · %llu 层容器 · %llu 层过滤器",
            static_cast<unsigned long long>(s.stats.filesOut.load()),
            format_size(s.stats.bytesOut.load()).c_str(),
            static_cast<unsigned long long>(s.stats.containers.load()),
            static_cast<unsigned long long>(s.stats.filters.load()));
    if (!dryRun) {
        log_out(" · 输入 %s", format_size(inB).c_str());
        if (inB > 0)
            log_out("（膨胀 %.1f×）", s.stats.bytesOut.load() / static_cast<double>(inB));
    }
    log_out("\n");
    if (s.stats.corrupt.load() || s.stats.branchesFailed.load()) {
        log_out("  失败分支 %d · 损坏条目 %d\n", s.stats.branchesFailed.load(),
                s.stats.corrupt.load());
    }
    log_out("  耗时 %.2fs\n", elapsedMs / 1000.0);

    int exitCode;
    if (cancelled)
        exitCode = 2;
    else if (s.stats.limitTripped.load())
        exitCode = 3;
    else if (s.stats.sawPasswordFail.load())
        exitCode = 2;
    else if (s.stats.sawMissingVol.load())
        exitCode = 4;
    else if (s.stats.sawCorrupt.load() || s.stats.branchesFailed.load())
        exitCode = 1;
    else
        exitCode = 0;

    // 仅资源管理器/右键启动（无标准句柄）→ GUI 完成反馈；取消则不弹（用户已决定）。
    // 管道/重定向（脚本、CLI）不弹框——有输出通道。
    bool explorerLaunched = GetStdHandle(STD_INPUT_HANDLE) == nullptr ||
                            GetStdHandle(STD_INPUT_HANDLE) == INVALID_HANDLE_VALUE;
    if (!dryRun && explorerLaunched && !cancelled) {
        std::string detail = exitCode == 0
            ? "已解出 " + std::to_string(s.stats.filesOut.load()) + " 个文件（" +
                  format_size(s.stats.bytesOut.load()) + "）\n输出: " + wide_to_utf8(outDir)
            : "存在错误（退出码 " + std::to_string(exitCode) + "），详见日志:\n" +
                  wide_to_utf8(log_path());
        gui::notify_done(exitCode == 0, detail);
    }
    return exitCode;
}
