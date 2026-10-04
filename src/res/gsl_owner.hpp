// res/gsl_owner.hpp —— 极简 gsl::owner 标注（零依赖自带）。
// 与 GSL 官方定义一致（模板别名，不改变类型）；clang-tidy 的
// cppcoreguidelines-owning-memory 按 ::gsl::owner 限定名识别。
// 用途：裸指针经标注显式声明"我拥有它，负责释放"——圈禁达成后 src/ 内
// owning 裸指针应趋零，仅存的点（如 res 工厂内部、进程级 COM 缓存的
// release() 转移端）必须带此标注，新增手工资源须先进 src/res/。
// 迁移注记：将来若引入真 ms-gsl/vcpkg 依赖，删除本文件即可——官方定义
// 与此逐字一致（同 TU 双 include 会是响亮的编译错，安全失败模式）。
#pragma once

namespace gsl {
template <typename T>
using owner = T;
}
