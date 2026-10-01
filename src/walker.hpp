// walker.hpp：递归策略引擎（设计 §4 Walker：分支语义 + Limiter + 密码约束）
#pragma once
#include "engines.hpp"
#include "password.hpp"
#include "session.hpp"
#include "sink.hpp"

namespace nx {

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
        return e;
    }
};

// 处理一个根输入（自动感知文件系统级分片）
void run_input(Session& s, const std::wstring& inputPath);

// 内部：walk 一条流（供递归；depth = 容器深度）。
// filterChain = 当前容器段内已叠套的过滤器层数（决策 D-1：过滤器链同样受 --depth
// 约束，进容器即重置——不能直接计入 depth，否则破坏 tar.gz 根的 noRoot 语义与层编号）。
// region：父视图区间（免 spool 直读；过滤器链剥离——解压后的字节无区间语义）
void walk(Session& s, std::unique_ptr<ByteSource> src, const std::string& sub,
          const std::string& origin, const std::string& chain, int depth, bool throughFilter,
          int filterChain, const std::shared_ptr<RegionSource>& region = nullptr);

} // namespace nx
