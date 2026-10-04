// container.hpp：容器读取器公共契约（engines 与 szcom 共用）
#pragma once
#include "bytesource.hpp"
#include "diag.hpp"
#include "format.hpp"
#include "password.hpp"
#include "spool.hpp"
#include <map>
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

// 迭代位置令牌：条目在读取器内的身份 = 单调迭代序号。
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

// 一个卷的数据来源（三选一）：文件系统路径 / spool 窗口（条目级多卷）/
// 父视图区间（嵌套容器免 spool 直读：stored 条目在父支撑中的连续字节）。
// 引擎契约层类型（原居 szcom.hpp：engines 门面签名需要它，放这里免得门面
// 渗漏 7z.dll 适配器的内部头）——szcom/open/walker 三方共用
struct VolumeSource {
    std::wstring fsPath;
    uint64_t fsBase = 0;   // FS 卷起始偏移（隐写窗口：文件 = [fsBase, EOF)）
    std::shared_ptr<SpoolBuffer> spool;   // 随窗口保活
    uint64_t winStart = 0, winLen = 0;
    std::shared_ptr<RegionSource> region;   // 父视图区间（与上两者互斥）
};

} // namespace nx
