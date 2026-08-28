// engines.hpp：容器引擎 —— libarchive（tar/cpio/ar/zip/iso/cab）+ 7z.dll（7z/rar）
#pragma once
#include "container.hpp"
#include "spool.hpp"
#include "szcom.hpp"
#include <map>
#include <memory>
#include <string>

namespace nx {

// 打开容器（接管 src 所有权）：
// - S/Z 类（tar/cpio/ar/zip）：libarchive 流式（zip 本地头流式）
// - R 类（7z/rar/iso/cab）：先 spool 全量再随机访问
//   · 7z/rar 优先 7z.dll（§7.2：7z 全特性 + RAR 解码），不可用/失败回退 libarchive
// - 加密层：候选密码迭代（§6.2）；zip 流式失败/SFX → D2 回退 spool+seek 重试
// 层身份 layerId 用于密码提示与缓存（如 "data.zip#inner.7z"）
std::shared_ptr<ContainerReader> open_container(std::unique_ptr<PushbackSource> src,
                                                Format fmt,
                                                const std::string& layerId,
                                                PasswordProvider& pw,
                                                const EngineOptions& opt);

// 原生多卷（RAR）打开：volumes 各卷数据（FS 路径或 spool 窗口），firstVol 主卷名。
// 走 7z.dll 卷回调路径（§3.3：原生卷型不拼接）。
std::shared_ptr<ContainerReader> open_container_volumes(
    Format fmt, const std::map<std::wstring, sz::VolumeSource>& volumes,
    const std::wstring& firstVol, const std::string& layerId, PasswordProvider& pw,
    const EngineOptions& opt);

} // namespace nx
