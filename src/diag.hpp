// diag.hpp：泄漏观测哨兵（S1-S10）
//
// 编译宏 NX_DIAG_LEAKS 启用（fuzz 目标常开，CMake option 亦可对 nx 主程序开启）；
// 关闭时全部调用为内联空体，零开销。纪律：泄漏验证只能用退出转储/注册表断言——
// 加日志会翻转泄漏的时序表现。
//
//   S1  SpoolBuffer 活性注册表      —— 进程退出/fuzz 每迭代断言全灭
//   S2  ContainerReader 活性注册表  —— 同上（LaSeq/SevenZip 经基类一处覆盖）
//   S3  try_open 失败出口           —— 本次创建的读取器必须已析构（D6 触发族 1
//                                      的案发现场检查，环泄漏在此直接 abort）
//   S4  ~ThreadPool                 —— join 后 q_.empty() && running_==0
//   S5  ~Sink                       —— INV-SINK：析构仅可发生在 waitAll 之后
#pragma once
#include <cstddef>

namespace nx::diag {

#ifdef NX_DIAG_LEAKS

void track_spool(const void* self);
void untrack_spool(const void* self);
void track_reader(const void* self);
void untrack_reader(const void* self);

// 注册表当前存活数（S3 基线用）
std::size_t live_readers();

// 非空即转储计数并 abort（fuzz 把 abort 当崩溃 → 泄漏回归即抓）
void check_all_destroyed(const char* where);

// S3：RAII 守卫——进入时记读取器基线；析构时若未标记 escaped（成功路径 reader
// 合法存活）则断言存活数回到基线。失败路径的读取器必须当场死透
struct TryOpenGuard {
    std::size_t base;
    bool escaped = false;
    TryOpenGuard();
    ~TryOpenGuard();
};

// S4/S5：断言失败即转储并 abort
void assert_true(bool cond, const char* what);

// 退出检查（main 经 atexit 安装；注册表为故意泄漏的单例，晚于一切静态析构）
void exit_check();

#else

inline void track_spool(const void*) {}
inline void untrack_spool(const void*) {}
inline void track_reader(const void*) {}
inline void untrack_reader(const void*) {}
inline std::size_t live_readers() { return 0; }
inline void check_all_destroyed(const char*) {}
struct TryOpenGuard {
    bool escaped = false;   // 保持字段布局一致；宏关闭时无行为
};
inline void assert_true(bool, const char*) {}
inline void exit_check() {}

#endif

} // namespace nx::diag
