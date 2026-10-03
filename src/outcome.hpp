// outcome.hpp：错误分类学 + 会话终态 → 退出码推导（纯核心，批次 1 领域 #4）
// 退出码契约（设计 §8 / AGENTS）：0 成功 | 1 部分失败 | 2 密码缺失或耗尽（含用户取消）
// | 3 超限熔断 | 4 缺分片 | 64 用法错误
// 错误家族原居 util.hpp（领域语义住进"工具"头，靠 util 是全量基座才无害）——
// 迁至本文件与终态推导同置：异常类型上的注释即退出码语义的映射表。
#pragma once
#include <expected>
#include <stdexcept>
#include <string>

namespace nx {

// 值错误二元表达（批次 1 试点，roadmap §6.3）：std::expected 别名隔离——
// 纯解析/冷路径优先；跨线程 exception_ptr、中止/熔断/密码耗尽控制流仍走异常
template <class T>
using Result = std::expected<T, std::string>;

// ---- 错误类型（决定退出码语义，见下方 derive_exit_code 与设计 §8）----
// NOLINT：sink 参数惯用法——按值收下再 move 入基类，非浪费拷贝
struct Error : std::runtime_error {
    explicit Error(std::string m) : std::runtime_error(std::move(m)) {}   // NOLINT(performance-unnecessary-value-param)
};
struct LimitError : Error {            // 超限熔断 / 磁盘水位 → 退出码 3
    explicit LimitError(std::string m) : Error(std::move(m)) {}
};
struct PasswordExhausted : Error {     // 密码缺失或耗尽 → 退出码 2
    std::string layer;
    PasswordExhausted(std::string layer_, std::string m)
        : Error(std::move(m)), layer(std::move(layer_)) {}
};
struct MissingVolumes : Error {        // 缺分片 → 退出码 4
    explicit MissingVolumes(std::string m) : Error(std::move(m)) {}
};
struct CorruptError : Error {          // 数据损坏（keep-going 可隔离）→ 记入退出码 1
    explicit CorruptError(std::string m) : Error(std::move(m)) {}
};
struct Cancelled : Error {             // 用户取消（GUI X/取消）→ 直接退出（M3 需求 5）
    explicit Cancelled(std::string m) : Error(std::move(m)) {}
};

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
