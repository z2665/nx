// container.hpp：容器读取器公共契约（engines 与 szcom 共用）
#pragma once
#include "bytesource.hpp"
#include "diag.hpp"
#include "format.hpp"
#include "password.hpp"
#include <memory>
#include <string>

namespace nx {

struct ContainerEntry {
    std::string name;
    uint64_t size = UINT64_MAX;      // 解压后大小；未知 = UINT64_MAX
    bool isDir = false;
    bool isSymlink = false;
    bool independentData = false;   // 数据源独立于迭代位置（spool/文件支撑）→ 可异步写出
    std::string symlinkTarget;
    std::shared_ptr<ByteSource> data;   // 顺序条目流：independent=false 时 next() 前有效
};

// 迭代位置令牌（领域 #10，批次 4）：条目在读取器内的身份 = 单调迭代序号。
// 契约（replayQ_ 环修复后的形态）：
//   · 预取/重放只携带 {token, meta}，绝不携带条目源——源在 next() 现场按 token 重建
//   · 条目源经 token 显式索取数据（readEntryData/readEntry），token 失效
//     （迭代已前进 / 读取器已销毁，weak_ptr 空）由读取器或源拒绝
//   · 读取器成员容器不得持有条目源（INV-3，所有权模型 NoLeak 的类型面）
struct EntryToken {
    uint64_t seq = 0;
};

class ContainerReader {
public:
    // S2 哨兵：活性登记在基类一处覆盖全部读取器（LaSeq/SevenZip…），宏关闭零开销
    ContainerReader() { diag::track_reader(this); }
    virtual ~ContainerReader() { diag::untrack_reader(this); }
    // false = 迭代结束。CorruptError（数据坏）/ PasswordExhausted（密码耗尽）经异常抛出。
    virtual bool next(ContainerEntry& out) = 0;
};

struct EngineOptions {
    size_t spoolRam = 64 << 20;
    std::wstring tempDir;
    InputMeter* meter = nullptr;   // 根输入计量（进度窗分母/分子；根层直读视图挂，spool 卷不挂）
};

} // namespace nx
