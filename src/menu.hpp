// menu.hpp：资源管理器右键菜单注册（M3 需求 2/3）—— HKCU，免管理员
#pragma once
#include <string>

namespace nx {

// 安装级联菜单「nx 解压」：解压到当前目录 / 解压到指定目录…
// 命令指向当前 nx.exe 的绝对路径（便携目录移动后需重新 install）
bool menu_install(std::string* errOut);
bool menu_remove(std::string* errOut);
bool menu_installed();   // 诊断

} // namespace nx
