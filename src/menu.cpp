#include "menu.hpp"
#include "log.hpp"
#include "util.hpp"
#include <windows.h>
#include <shlwapi.h>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shlwapi.lib")

namespace nx {

namespace {

// 父菜单键（HKCU，免管理员）
const wchar_t* kParent = L"Software\\Classes\\*\\shell\\nxExtract";
// 子命令注册到 CommandStore（微软文档的级联实现方式：
// learn.microsoft.com/windows/win32/shell/context-menus — Creating Static Cascading Menus）
const wchar_t* kStoreRoot =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell";
const wchar_t* kStoreKeys[2] = {L"nx.here", L"nx.into"};

std::wstring exe_path() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    return std::wstring(buf, n);
}

bool set_reg(HKEY root, const wchar_t* sub, const wchar_t* value, const wchar_t* data) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return false;
    bool ok = RegSetValueExW(k, value, 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(data),
                             static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t))) ==
              ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

bool del_tree(HKEY root, const wchar_t* sub) {
    std::wstring parentPath, leaf;
    const wchar_t* s = wcsrchr(sub, L'\\');
    if (s) {
        parentPath.assign(sub, s - sub);
        leaf = s + 1;
    } else {
        leaf = sub;
    }
    HKEY h = nullptr;
    LSTATUS r = parentPath.empty()
                    ? RegOpenKeyExW(root, nullptr, 0, DELETE, &h)
                    : RegOpenKeyExW(root, parentPath.c_str(), 0, DELETE, &h);
    if (r != ERROR_SUCCESS)
        return true;   // 路径不存在 = 已删
    LSTATUS d = SHDeleteKeyW(h, leaf.c_str());
    RegCloseKey(h);
    return d == ERROR_SUCCESS || d == ERROR_FILE_NOT_FOUND;
}

} // namespace

bool menu_install(std::string* errOut) {
    std::wstring exe = exe_path();
    if (exe.empty()) {
        if (errOut) *errOut = "无法定位 nx.exe 路径";
        return false;
    }
    // 清理旧形态（平级两项 / 旧级联）
    del_tree(HKEY_CURRENT_USER, kParent);
    del_tree(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\nxExtractHere");
    del_tree(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\nxExtractInto");
    for (const wchar_t* k : kStoreKeys)
        del_tree(HKEY_CURRENT_USER, (std::wstring(kStoreRoot) + L"\\" + k).c_str());

    bool ok = true;
    // 1) 子命令进 CommandStore（级联展开的内容）
    struct Sub {
        const wchar_t* key;
        const wchar_t* title;
        const wchar_t* arg;
    } subs[2] = {
        {L"nx.here", L"解压到当前目录", L"extract-here"},
        {L"nx.into", L"解压到指定目录…", L"extract-into"},
    };
    for (auto& s : subs) {
        std::wstring key = std::wstring(kStoreRoot) + L"\\" + s.key;
        ok &= set_reg(HKEY_CURRENT_USER, key.c_str(), nullptr, s.title);
        std::wstring cmd = L"\"" + exe + L"\" " + s.arg + L" \"%1\"";
        ok &= set_reg(HKEY_CURRENT_USER, (key + L"\\command").c_str(), nullptr, cmd.c_str());
        ok &= set_reg(HKEY_CURRENT_USER, key.c_str(), L"Icon", (exe + L",0").c_str());
    }
    // 2) 父菜单：MUIVerb + SubCommands 引用 CommandStore 条目
    ok &= set_reg(HKEY_CURRENT_USER, kParent, L"MUIVerb", L"nx 解压");
    ok &= set_reg(HKEY_CURRENT_USER, kParent, L"Icon", (exe + L",0").c_str());
    ok &= set_reg(HKEY_CURRENT_USER, kParent, L"SubCommands", L"nx.here;nx.into");

    if (!ok && errOut)
        *errOut = "注册表写入失败";
    return ok;
}

bool menu_remove(std::string* errOut) {
    bool ok = del_tree(HKEY_CURRENT_USER, kParent);
    for (const wchar_t* k : kStoreKeys)
        ok &= del_tree(HKEY_CURRENT_USER, (std::wstring(kStoreRoot) + L"\\" + k).c_str());
    ok &= del_tree(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\nxExtractHere");
    ok &= del_tree(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\nxExtractInto");
    if (!ok && errOut)
        *errOut = "删除失败";
    return ok;
}

bool menu_installed() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kParent, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}

} // namespace nx
