// engines.hpp：容器引擎门面 —— libarchive（tar/cpio/ar/zip/iso/cab）+ 7z.dll（7z/rar）
// 实现在 open.cpp（组合根：laseq/zipcd/szcom 的装配与回退策略）。
// 门面不 include szcom.hpp——7z.dll 适配器类型不渗漏给下游（walker 经本头开卷与探测）
#pragma once
#include "container.hpp"
#include "spool.hpp"
#include <map>
#include <memory>
#include <string>

namespace nx {

// 打开容器（接管 src 所有权）：
// - S/Z 类（tar/cpio/ar/zip）：libarchive 流式（zip 本地头流式）
// - R 类（7z/rar/iso/cab）：先 spool 全量再随机访问
//   · 7z/rar 优先 7z.dll（§7.2：7z 全特性 + RAR 解码），不可用/失败回退 libarchive
// - 加密层：候选密码迭代（§6.2）；zip 流式失败/SFX → D2 回退 spool+seek 重试
// region：可选——父视图中的连续 stored 区间（免 spool 窗口直读，见 bytesource.hpp
// RegionSource）；zip/R 类命中时直接在区间上开，失败自动回退 spool 原路径
// 层身份 layer（LayerId：key = 逻辑路径缓存键，display = 提示文本；批次 2）
std::shared_ptr<ContainerReader> open_container(std::unique_ptr<PushbackSource> src,
                                                Format fmt,
                                                const LayerId& layer,
                                                PasswordProvider& pw,
                                                const EngineOptions& opt,
                                                const std::shared_ptr<RegionSource>& region = nullptr);

// 原生多卷（RAR）打开：volumes 各卷数据（FS 路径或 spool 窗口），firstVol 主卷名。
// 走 7z.dll 卷回调路径（§3.3：原生卷型不拼接）。
std::shared_ptr<ContainerReader> open_container_volumes(
    Format fmt, const std::map<std::wstring, VolumeSource>& volumes,
    const std::wstring& firstVol, const LayerId& layer, PasswordProvider& pw,
    const EngineOptions& opt);

// 7z.dll 可用性探测（含惰性加载语义：首次调用即尝试加载）。walker 的路由决策用
// （fsDirectOpen 快路径 / 隐写 7z-rar 路径的门禁）；实际开卷一律走上面两个入口。
bool sevenzip_dll_available();


// Zip 根文件直读（中央目录 + 码表探测，免 spool）。
// base/length：隐写窗口（EOCD 精确区间，排除尾部伪装）；默认 0 = 整文件。
std::shared_ptr<ContainerReader> open_zip_file(const std::wstring& path,
                                               const LayerId& layer,
                                               PasswordProvider& pw,
                                               const EngineOptions& opt,
                                               uint64_t base = 0,
                                               uint64_t length = 0);

} // namespace nx
