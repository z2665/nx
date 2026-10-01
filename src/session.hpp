// session.hpp：全局选项 / 统计 / 会话对象
#pragma once
#include "util.hpp"
#include <atomic>
#include <mutex>
#include <string>

namespace nx {

struct Options {
    int maxDepth = 10;   // 容器嵌套深度与每容器段内过滤器链长的共用上限（决策 D-1）
    uint64_t maxBytes = 512ull << 30;   // 累计输出上限
    uint64_t maxRatio = 1000;           // 压缩比熔断（产出/输入）
    size_t spoolRam = 0;             // 0 = 自动（main 探测空闲物理内存 50%，64MiB–8GiB）
    size_t pipeBytes = 1 << 20;         // 级间有界队列容量
    size_t histCap = 4 << 20;           // 回看窗口
    bool keepGoing = false;             // 数据损坏时继续其余条目
    bool dryRun = false;                // tree 模式
    std::string verify;                 // "sha256"（D8：--verify sha256）
    bool noRoot = false;                // 不建根目录层（右键"解压到当前目录/前缀"语义）
    bool guiPrompt = false;             // 密码经 GUI 弹窗（--gui；无控制台时自动）
    bool stegoMode = false;             // 隐写模式（extract-stego / --stego）：只解根文件内藏归档
};

// 首个硬错误消息（D3）：写出线程池写、Walker/迭代线程读——std::string 跨线程
// 无锁读写是数据竞争 UB，槽内自带互斥（锁内快照语义）
struct HardErrorSlot {
    void set(const std::string& msg) {
        std::lock_guard<std::mutex> lk(m);
        if (s.empty()) s = msg;
    }
    std::string get() const {
        std::lock_guard<std::mutex> lk(m);
        return s;
    }
private:
    mutable std::mutex m;
    std::string s;
};

struct Stats {
    std::atomic<uint64_t> filesOut{0};
    std::atomic<uint64_t> bytesOut{0};
    std::atomic<uint64_t> inputTotal{0};     // 根输入总大小（进度窗分母；run_input 累计）
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
    std::atomic<bool> stegoNotFound{false};  // 隐写模式未命中（exit 0 + GUI 提示，不算失败）
    std::atomic<bool> abortFlag{false};         // Sink 硬错误 → 全局终止
    HardErrorSlot firstHardError;
};

} // namespace nx
