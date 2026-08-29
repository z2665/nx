// session.hpp：全局选项 / 统计 / 会话对象
#pragma once
#include "util.hpp"
#include <atomic>
#include <string>

namespace nx {

struct Options {
    int maxDepth = 8;
    uint64_t maxBytes = 512ull << 30;   // 累计输出上限
    uint64_t maxRatio = 1000;           // 压缩比熔断（产出/输入）
    size_t spoolRam = 64 << 20;
    size_t pipeBytes = 1 << 20;         // 级间有界队列容量
    size_t histCap = 4 << 20;           // 回看窗口
    bool keepGoing = false;             // 数据损坏时继续其余条目
    bool dryRun = false;                // tree 模式
    std::string verify;                 // "sha256"（D8：--verify sha256）
    bool noRoot = false;                // 不建根目录层（右键"解压到当前目录/前缀"语义）
    bool guiPrompt = false;             // 密码经 GUI 弹窗（--gui；无控制台时自动）
};

struct Stats {
    std::atomic<uint64_t> filesOut{0};
    std::atomic<uint64_t> bytesOut{0};
    std::atomic<uint64_t> tempBytes{0};
    std::atomic<uint64_t> produced{0};          // 过滤器累计产出（压缩比分子）
    std::atomic<uint64_t> containers{0};
    std::atomic<uint64_t> filters{0};
    std::atomic<int> corrupt{0};
    std::atomic<int> branchesFailed{0};
    std::atomic<bool> sawPasswordFail{false};
    std::atomic<bool> sawMissingVol{false};
    std::atomic<bool> sawCorrupt{false};
    std::atomic<bool> limitTripped{false};
    std::atomic<bool> abortFlag{false};         // Sink 硬错误 → 全局终止
    std::string firstHardError;
};

} // namespace nx
