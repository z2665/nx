// fuzz_main.cpp：libFuzzer 全管线目标（安全护城河——对抗性输入的崩溃/内存安全底线）
//
// 设计（见 README「Fuzz 安全护城河」）：
// - 每迭代把输入写临时文件 → run_input 走真实管线（detect/stego/容器引擎/密码链/
//   Walker 递归/Sink 消毒落盘），覆盖面 = 生产路径本身，而非仿制解析循环；
// - 限额收紧：深度 3 / 输出 2MiB / 压缩比 50 / spool RAM 1MiB（促发磁盘溢出分支）——
//   炸弹在熔断处终止，单迭代成本与磁盘占用有界；
// - 输出目录 4 槽轮换、先清后用；spool 溢出临时文件都在 %TEMP%\nxfuzz 下；
// - stegoMode/noRoot 由输入尺寸奇偶派生（确定性——libFuzzer 依赖可复现执行；
//   变异改变尺寸即自然覆盖两条路径）；
// - 不调用 log_open（无 nx.log）+ log_set_quiet 抑制控制台；密码候选固定三枚且不交互。
#include "walker.hpp"
#include "diag.hpp"
#include "log.hpp"

#include <atomic>
#include <clocale>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;

namespace {

fs::path g_root;                       // %TEMP%\nxfuzz
std::atomic<uint64_t> g_seq{0};

constexpr size_t kMaxInput = 4ull << 20;   // 超大输入跳过（-max_len 另有限制）

} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    setlocale(LC_ALL, ".UTF8");   // 与 nx main 一致：libarchive 非 ASCII 名依赖进程 locale
    nx::log_set_quiet(true);
    std::error_code ec;
    g_root = fs::temp_directory_path(ec) / "nxfuzz";
    if (ec) g_root = fs::path(L"nxfuzz");
    fs::create_directories(g_root / L"tmp", ec);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > kMaxInput) return 0;

    uint64_t n = g_seq.fetch_add(1);
    fs::path in = g_root / L"input.bin";
    {
        std::ofstream f(in, std::ios::binary | std::ios::trunc);   // trunc：收缩上次更大输入的尾巴
        if (!f) return 0;
        f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    fs::path out = g_root / L"out" / std::to_string(n % 4);
    std::error_code ec;
    fs::remove_all(out, ec);
    fs::create_directories(out, ec);

    {
        nx::Session s;
        s.opt.maxDepth = 3;
        s.opt.maxBytes = 2ull << 20;
        s.opt.maxRatio = 50;
        s.opt.spoolRam = 1ull << 20;      // 小 RAM：促发 spool 磁盘溢出分支
        s.opt.histCap = 512ull << 10;
        s.opt.pipeBytes = 64ull << 10;
        s.opt.keepGoing = true;
        s.opt.stegoMode = (size % 2) == 0;    // 尺寸派生：隐写路径
        s.opt.noRoot = ((size / 2) % 2) == 0;
        s.pw.addCandidate("infected");
        s.pw.addCandidate("a");
        s.pw.addCandidate("123456");
        s.pw.setNoPrompt(true);
        s.tempDir = (g_root / L"tmp").wstring();
        s.sink = std::make_unique<nx::Sink>(out.wstring(), s.opt, s.stats, false);

        try {
            nx::run_input(s, in.wstring());
        } catch (...) {   // 业务错误（Corrupt/Limit/PasswordExhausted/MissingVolumes…）都是合法结局
        }
        try {
            s.sink->waitAll();
        } catch (...) {
        }
    }
    // S1/S2 哨兵（fuzz 常开）：每迭代断言 spool/读取器全灭——任何泄漏形态
    // （含 D6 两触发族）在此 abort，libFuzzer 当崩溃收
    nx::diag::check_all_destroyed("fuzz-iteration");
    return 0;
}
