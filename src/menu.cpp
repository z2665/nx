#include "menu.hpp"
#include "log.hpp"
#include "util.hpp"
#include <windows.h>
#include <shlwapi.h>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shlwapi.lib")

namespace nx {

namespace {

// 平级两项（M3：级联 SubCommands 在部分 Windows 版本不展开子菜单，平级 100% 可靠）
const wchar_t* kKeys[2] = {
    L"Software\\Classes\\*\\shell\\nxExtractHere",
    L"Software\\Classes\\*\\shell\\nxExtractInto",
};
// 旧版级联键（升级清理）
const wchar_t* kLegacy = L"Software\\Classes\\*\\shell\\nxExtract";

std::wstring exe_path() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    return std::wstring(buf, n);
}

bool set_reg(HKEY parent, const wchar_t* sub, const wchar_t* value, const wchar_t* data) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(parent, sub, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return false;
    bool ok = RegSetValueExW(k, value, 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(data),
                             static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t))) ==
              ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

bool del_tree(const wchar_t* shellParent, const wchar_t* sub) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, shellParent, 0, DELETE, &k) != ERROR_SUCCESS)
        return true;   // 不存在即成功
    LSTATUS r = SHDeleteKeyW(k, sub);
    RegCloseKey(k);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

} // namespace

bool menu_install(std::string* errOut) {
    std::wstring exe = exe_path();
    if (exe.empty()) {
        if (errOut) *errOut = "无法定位 nx.exe 路径";
        return false;
    }
    del_tree(L"Software\\Classes\\*\\shell", L"nxExtract");   // 清理旧级联键
    bool ok = true;
    struct Item {
        const wchar_t* key;
        const wchar_t* title;
        const wchar_t* verbArg;
    } items[2] = {
        {kKeys[0], L"nx 解压到当前目录", L"extract-here"},
        {kKeys[1], L"nx 解压到指定目录…", L"extract-into"},
    };
    std::wstring icon = exe + L",0";
    for (auto& it : items) {
        ok &= set_reg(HKEY_CURRENT_USER, it.key, nullptr, it.title);
        ok &= set_reg(HKEY_CURRENT_USER, it.key, L"Icon", icon.c_str());
        std::wstring cmd = L"\"" + exe + L"\" " + it.verbArg + L" \"%1\"";
        ok &= set_reg(HKEY_CURRENT_USER, (std::wstring(it.key) + L"\\command").c_str(), nullptr,
                      cmd.c_str());
    }
    if (!ok && errOut)
        *errOut = "注册表写入失败";
    return ok;
}

bool menu_remove(std::string* errOut) {
    bool ok = del_tree(L"Software\\Classes\\*\\shell", L"nxExtractHere");
    ok &= del_tree(L"Software\\Classes\\*\\shell", L"nxExtractInto");
    ok &= del_tree(L"Software\\Classes\\*\\shell", L"nxExtract");
    if (!ok && errOut)
        *errOut = "删除失败";
    return ok;
}

bool menu_installed() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKeys[0], 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}

} // namespace nx
