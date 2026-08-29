#include "menu.hpp"
#include "log.hpp"
#include "util.hpp"
#include <windows.h>
#include <shlwapi.h>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shlwapi.lib")

namespace nx {

namespace {

// HKCU\Software\Classes\*\shell\nxExtract（级联：SubCommands）
const wchar_t* kRoot = L"Software\\Classes\\*\\shell\\nxExtract";

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

} // namespace

bool menu_install(std::string* errOut) {
    std::wstring exe = exe_path();
    if (exe.empty()) {
        if (errOut) *errOut = "无法定位 nx.exe 路径";
        return false;
    }
    bool ok = true;
    ok &= set_reg(HKEY_CURRENT_USER, kRoot, L"MUIVerb", L"nx 解压");
    std::wstring icon = exe + L",0";
    ok &= set_reg(HKEY_CURRENT_USER, kRoot, L"Icon", icon.c_str());
    ok &= set_reg(HKEY_CURRENT_USER, kRoot, L"SubCommands", L"nx.here;nx.into");
    ok &= set_reg(HKEY_CURRENT_USER, (std::wstring(kRoot) + L"\\shell\\nx.here").c_str(),
                  nullptr, L"解压到当前目录");
    std::wstring cmdHere = L"\"" + exe + L"\" extract-here \"%1\"";
    ok &= set_reg(HKEY_CURRENT_USER,
                  (std::wstring(kRoot) + L"\\shell\\nx.here\\command").c_str(), nullptr,
                  cmdHere.c_str());
    ok &= set_reg(HKEY_CURRENT_USER, (std::wstring(kRoot) + L"\\shell\\nx.into").c_str(),
                  nullptr, L"解压到指定目录…");
    std::wstring cmdInto = L"\"" + exe + L"\" extract-into \"%1\"";
    ok &= set_reg(HKEY_CURRENT_USER,
                  (std::wstring(kRoot) + L"\\shell\\nx.into\\command").c_str(), nullptr,
                  cmdInto.c_str());
    if (!ok && errOut)
        *errOut = "注册表写入失败";
    return ok;
}

bool menu_remove(std::string* errOut) {
    HKEY classes = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell", 0, DELETE,
                      &classes) != ERROR_SUCCESS) {
        if (errOut) *errOut = "打开注册表失败";
        return false;
    }
    LSTATUS r = SHDeleteKeyW(classes, L"nxExtract");
    RegCloseKey(classes);
    if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) {
        if (errOut) *errOut = "删除失败 (code " + std::to_string(static_cast<long>(r)) + ")";
        return false;
    }
    return true;
}

bool menu_installed() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRoot, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}

} // namespace nx
