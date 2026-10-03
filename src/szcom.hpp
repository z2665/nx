// szcom.hpp：7z.dll（IInArchive COM）适配层 —— 7z 全特性 + RAR 解码（设计 §7.2）
// 接口与 GUID 依 7-Zip SDK（Guid.txt：接口 {23170F69-40C1-278A-0000-00yy00xx0000}，
// 处理器 {23170F69-40C1-278A-1000-000110xx0000}）。
#pragma once
#include "container.hpp"
#include "spool.hpp"
#include <map>
#include <memory>
#include <string>

namespace nx::sz {

// DLL 可用性（进程内首次调用时加载：exe 目录 → 7-Zip 安装目录 → PATH）
bool dll_available();
std::wstring dll_path();     // 实际加载路径（诊断）
std::wstring dll_error();    // 加载失败原因

// VolumeSource（卷数据来源，三选一：FS 路径 / spool 窗口 / 父视图区间）
// 定义在 container.hpp（引擎契约层，与适配层解耦）

// 打开 7z / rar（fmt ∈ {SevenZip, Rar}）。
// volumes：卷名（basename，大小写不敏感）→ 数据；无卷名的单卷用 firstVol=L""。
// firstVol：主卷名（RAR 卷回调 GetProperty(kpidName) 返回它，7z.dll 据此推兄弟卷名）。
// 密码：候选迭代（§6.2）；耗尽抛 PasswordExhausted，数据坏抛 CorruptError。
std::shared_ptr<ContainerReader> open_archive(Format fmt,
                                              const std::map<std::wstring, VolumeSource>& volumes,
                                              const std::wstring& firstVol,
                                              const LayerId& layer,
                                              PasswordProvider& pw,
                                              const EngineOptions& opt);

} // namespace nx::sz
