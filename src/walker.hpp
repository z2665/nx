// walker.hpp：递归策略引擎（设计 §4 Walker：分支语义 + Limiter + 密码约束）
// 递归主干收敛为 Walker 类；运行期选项装配单点 resolve_runtime_options
#pragma once
#include "engines.hpp"
#include "layer.hpp"
#include "password.hpp"
#include "session.hpp"
#include "sink.hpp"

namespace nx {

// 会话聚合根：定义于此而非 session.hpp（后者被 sink.hpp 依赖，反向放置需
// 前置声明+外置析构绕路——见 session.hpp 头注）
class Session {
public:
    Options opt;
    Stats stats;
    PasswordProvider pw;
    InputMeter meter;   // 根输入计量（压缩比熔断分母，D6）
    std::unique_ptr<Sink> sink;
    std::wstring tempDir;

    EngineOptions engineOpt() {   // 非 const：meter 需以可写指针透传给引擎（计数）
        EngineOptions e;
        e.spoolRam = opt.spoolRam;
        e.tempDir = tempDir;
        e.meter = &meter;
        e.spoolDiskCap = opt.maxBytes;   // M6：spool 溢出总量与累计输出同限
        return e;
    }
};

// 递归策略引擎（分片感知 + stego 分派 + 免 spool 直读快路径）。
// 递归主干（walk/iterate/process/flush，LayerCtx 传递）是 walker.cpp 内部自由函数——
// PendingSet 为其匿名命名空间实现细节，不入头文件
class Walker {
public:
    explicit Walker(Session& s) : s_(s) {}

    // 处理一个根输入（自动感知文件系统级分片；stegoMode 分派到隐写路径）
    void run(const std::wstring& inputPath);

private:
    bool fsDirectOpen(const std::wstring& path, const std::string& rootName);
    void runStego(const std::wstring& inputPath);

    Session& s_;
};

// 兼容自由入口（main / fuzz 调用形态不变）
void run_input(Session& s, const std::wstring& inputPath);

// 运行期选项装配单点：spool RAM 自适应（空闲物理内存 50%，64MiB–8GiB）
// + 溢出临时目录默认=输出目录（tree 无输出目录 → 系统临时目录）
void resolve_runtime_options(Options& opt, std::wstring& tempDir,
                             const std::wstring& outDir, bool dryRun);

} // namespace nx
