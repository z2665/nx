// outcome.hpp：会话终态 → 退出码推导（纯函数，roadmap 领域 #4 / 批次 1）
// 退出码契约（设计 §8 / AGENTS）：0 成功 | 1 部分失败 | 2 密码缺失或耗尽（含用户取消）
// | 3 超限熔断 | 4 缺分片 | 64 用法错误
#pragma once

namespace nx {

// 会话终态快照（从 Stats 原子量一次性gather——决策层只看值，不看并发原语）
struct OutcomeFlags {
    bool cancelled = false;
    bool limitTripped = false;
    bool passwordFail = false;
    bool missingVolume = false;
    bool corrupt = false;
    int branchesFailed = 0;
};

// 优先级：取消 > 超限 > 密码 > 缺分片 > 部分失败 > 成功（与原 main if 链逐分支等价）
inline int derive_exit_code(const OutcomeFlags& f) {
    if (f.cancelled) return 2;
    if (f.limitTripped) return 3;
    if (f.passwordFail) return 2;
    if (f.missingVolume) return 4;
    if (f.corrupt || f.branchesFailed > 0) return 1;
    return 0;
}

} // namespace nx
