#include "menu.hpp"
#include "log.hpp"
#include "util.hpp"
#include <windows.h>
#include <shlwapi.h>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shlwapi.lib")

namespace nx {

namespace {

// 级联方式：ExtendedSubCommandsKey（微软 Static Cascasing Menus 的第二种形式）。
// CommandStore 仅 HKLM 受支持（HKCU 下 SubCommands 解析不到 → 前版不展开的根因）；
// ExtendedSubCommandsKey 指向自定义级联键，纯 HKCU 可用。
const wchar_t* kParent = L"Software\\Classes\\*\\shell\\nxExtract";
const wchar_t* kCascadeRoot = L"Software\\Classes\\nx.ContextMenu";   // 子动词宿主
const wchar_t* kLegacyStore =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell";

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
    // 清理旧形态（平级两项 / CommandStore 级联）
    del_tree(HKEY_CURRENT_USER, kParent);
    del_tree(HKEY_CURRENT_USER, kCascadeRoot);
    del_tree(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\nxExtractHere");
    del_tree(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\nxExtractInto");
    del_tree(HKEY_CURRENT_USER, (std::wstring(kLegacyStore) + L"\\nx.here").c_str());
    del_tree(HKEY_CURRENT_USER, (std::wstring(kLegacyStore) + L"\\nx.into").c_str());

    bool ok = true;
    // 1) 子动词挂在自定义级联键下（HKCR 相对路径 nx.ContextMenu）
    struct Sub {
        const wchar_t* key;
        const wchar_t* title;
        const wchar_t* arg;
    } subs[3] = {
        {L"nx.here", L"解压到当前目录", L"extract-here"},
        {L"nx.into", L"解压到指定目录…", L"extract-into"},
        {L"nx.stego", L"解压隐写压缩包…", L"extract-stego"},
    };
    for (auto& s : subs) {
        std::wstring key = std::wstring(kCascadeRoot) + L"\\shell\\" + s.key;
        ok &= set_reg(HKEY_CURRENT_USER, key.c_str(), nullptr, s.title);
        std::wstring cmd = L"\"" + exe + L"\" " + s.arg + L" \"%1\"";
        ok &= set_reg(HKEY_CURRENT_USER, (key + L"\\command").c_str(), nullptr, cmd.c_str());
        ok &= set_reg(HKEY_CURRENT_USER, key.c_str(), L"Icon", (exe + L",0").c_str());
    }
    // 2) 父菜单：MUIVerb + ExtendedSubCommandsKey 指向级联键（相对 HKCR）
    ok &= set_reg(HKEY_CURRENT_USER, kParent, L"MUIVerb", L"nx 解压");
    ok &= set_reg(HKEY_CURRENT_USER, kParent, L"Icon", (exe + L",0").c_str());
    ok &= set_reg(HKEY_CURRENT_USER, kParent, L"ExtendedSubCommandsKey", L"nx.ContextMenu");

    if (!ok && errOut)
        *errOut = "注册表写入失败";
    return ok;
}

bool menu_remove(std::string* errOut) {
    bool ok = del_tree(HKEY_CURRENT_USER, kParent);
    ok &= del_tree(HKEY_CURRENT_USER, kCascadeRoot);
    ok &= del_tree(HKEY_CURRENT_USER, (std::wstring(kLegacyStore) + L"\\nx.here").c_str());
    ok &= del_tree(HKEY_CURRENT_USER, (std::wstring(kLegacyStore) + L"\\nx.into").c_str());
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
