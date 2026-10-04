// nxshell.cpp：Windows 11 新版右键菜单扩展
//
// IExplorerCommand COM DLL + 稀疏 MSIX 包（ExternalLocation 指向 dist\nx）：
//   父命令「nx 解压」（EnumSubCommands 提供子命令 → 新版右键级联）
//     ├─ 解压到当前目录  → nx.exe extract-here <所选文件>
//     └─ 解压到指定目录… → nx.exe extract-into  <所选文件>
//
// 依官方指南：learn.microsoft.com/windows/apps/desktop/modernize/
//             integrate-packaged-app-with-file-explorer
// WIN32_LEAN_AND_MEAN/NOMINMAX 由 CMake 全局定义，此处不再重复（曾致 C4005）
#include <windows.h>
#include <shobjidl_core.h>
#include <shlwapi.h>
#include <objbase.h>
#include <initguid.h>   // DEFINE_GUID 生成定义（而非 extern 声明）
#include "res/unique_handle.hpp"   // P2 圈禁：句柄/COM RAII
#include "res/com_ptr.hpp"
#include <cstring>
#include <string>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "User32.lib")

// 本 DLL 不在 nx 命名空间内——RAII 类型经别名引用（P2 圈禁）
namespace res = ::nx::res;

// {7A3E9C41-5B2D-4E8A-9F60-3C1D84B2A501} 父命令（级联入口）
DEFINE_GUID(CLSID_NxMenu, 0x7a3e9c41, 0x5b2d, 0x4e8a, 0x9f, 0x60, 0x3c, 0x1d, 0x84,
            0xb2, 0xa5, 0x01);
// {7A3E9C41-5B2D-4E8A-9F60-3C1D84B2A502} 解压到当前目录
DEFINE_GUID(CLSID_NxHere, 0x7a3e9c41, 0x5b2d, 0x4e8a, 0x9f, 0x60, 0x3c, 0x1d, 0x84,
            0xb2, 0xa5, 0x02);
// {7A3E9C41-5B2D-4E8A-9F60-3C1D84B2A503} 解压到指定目录…
DEFINE_GUID(CLSID_NxInto, 0x7a3e9c41, 0x5b2d, 0x4e8a, 0x9f, 0x60, 0x3c, 0x1d, 0x84,
            0xb2, 0xa5, 0x03);

static HMODULE g_hMod = nullptr;

static std::wstring dll_dir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(g_hMod, buf, MAX_PATH * 4);
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L'\\');
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

static std::wstring nx_exe_path() {
    return dll_dir() + L"\\nx.exe";
}

// 取第一个所选条目的文件系统路径
static std::wstring first_selected_path(IShellItemArray* items) {
    if (!items)
        return {};
    res::com_ptr<IShellItem> item;
    if (FAILED(items->GetItemAt(0, item.out())) || !item)
        return {};
    PWSTR p = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        std::wstring r = p;
        CoTaskMemFree(p);
        return r;
    }
    return {};
}

static void launch_nx(const wchar_t* verb, IShellItemArray* items) {
    std::wstring file = first_selected_path(items);
    if (file.empty())
        return;
    std::wstring cmd = L"\"" + nx_exe_path() + L"\" " + verb + L" \"" + file + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       dll_dir().c_str(), &si, &pi)) {
        // P2 圈禁：进程/线程句柄 RAII——作用域结束自动关闭
        res::UniqueKernelObject piThread(pi.hThread), piProcess(pi.hProcess);
    }
}

// ---------------- IExplorerCommand 实现 ----------------

class NxCommand : public IExplorerCommand {
public:
    NxCommand(const GUID* clsid) : clsid_(clsid) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --ref_;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IExplorerCommand)) {
            *ppv = static_cast<IExplorerCommand*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IExplorerCommand
    HRESULT STDMETHODCALLTYPE GetTitle(IShellItemArray*, PWSTR* name) override {
        if (!name) return E_POINTER;
        if (clsid_ == &CLSID_NxMenu) return SHStrDupW(L"nx 解压", name);
        if (clsid_ == &CLSID_NxHere) return SHStrDupW(L"解压到当前目录", name);
        return SHStrDupW(L"解压到指定目录…", name);
    }
    HRESULT STDMETHODCALLTYPE GetIcon(IShellItemArray*, PWSTR* icon) override {
        if (!icon) return E_POINTER;
        return SHStrDupW((nx_exe_path() + L",0").c_str(), icon);
    }
    HRESULT STDMETHODCALLTYPE GetToolTip(IShellItemArray*, PWSTR* tip) override {
        if (!tip) return E_POINTER;
        if (clsid_ == &CLSID_NxHere) return SHStrDupW(L"解压嵌套压缩包，内容放在压缩文件所在目录", tip);
        if (clsid_ == &CLSID_NxInto) return SHStrDupW(L"弹窗输入前缀目录名后解压", tip);
        *tip = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalName(GUID* name) override {
        if (!name) return E_POINTER;
        *name = *clsid_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetState(IShellItemArray*, BOOL, EXPCMDSTATE* state) override {
        if (!state) return E_POINTER;
        *state = ECS_ENABLED;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(IShellItemArray* items, IBindCtx*) override {
        if (clsid_ == &CLSID_NxHere)
            launch_nx(L"extract-here", items);
        else if (clsid_ == &CLSID_NxInto)
            launch_nx(L"extract-into", items);
        return S_OK;   // 父命令（级联）不会被 Invoke
    }
    HRESULT STDMETHODCALLTYPE GetFlags(EXPCMDFLAGS* flags) override {
        if (!flags) return E_POINTER;
        *flags = ECF_DEFAULT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumSubCommands(IEnumExplorerCommand** cmds) override {
        if (!cmds) return E_POINTER;
        if (clsid_ != &CLSID_NxMenu) {
            *cmds = nullptr;
            return E_NOTIMPL;
        }
        // 级联子命令：当前目录 / 指定目录（新右键的 flyout）
        *cmds = MakeEnumerator({new NxCommand(&CLSID_NxHere), new NxCommand(&CLSID_NxInto)});
        return S_OK;
    }

private:
    static IEnumExplorerCommand* MakeEnumerator(std::initializer_list<IExplorerCommand*> list);

    const GUID* clsid_;
    ULONG ref_ = 1;
};

class NxEnum : public IEnumExplorerCommand {
public:
    NxEnum(IExplorerCommand** arr, ULONG n) : n_(n), i_(0) {
        for (ULONG i = 0; i < n; ++i) arr_[i] = res::com_ptr(arr[i]);   // 接管引用
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --ref_;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumExplorerCommand)) {
            *ppv = static_cast<IEnumExplorerCommand*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG celt, IExplorerCommand** elt, ULONG* fetched) override {
        if (!elt) return E_POINTER;
        ULONG got = 0;
        while (got < celt && i_ < n_) {
            elt[got] = arr_[i_++].get();
            elt[got]->AddRef();
            ++got;
        }
        if (fetched) *fetched = got;
        return got == celt ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override {
        i_ += celt;
        return i_ <= n_ ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        i_ = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IEnumExplorerCommand** ppm) override {
        if (!ppm) return E_POINTER;
        *ppm = nullptr;
        return E_NOTIMPL;
    }

private:
    res::com_ptr<IExplorerCommand> arr_[8];   // P2 圈禁：析构自动 Release
    ULONG n_, i_, ref_ = 1;
};

IEnumExplorerCommand* NxCommand::MakeEnumerator(std::initializer_list<IExplorerCommand*> list) {
    IExplorerCommand* arr[8] = {};
    ULONG n = 0;
    for (auto* p : list) {
        if (n < 8) arr[n++] = p;   // 保留调用方引用（new 出来的 ref=1），枚举器释放时 Release
    }
    // NOLINT：COM 边界——引用计数随裸指针移交（ref=1 交给调用方，Release 即释放）
    return new NxEnum(arr, n);   // NOLINT(cppcoreguidelines-owning-memory)
}

// ---------------- COM 工厂 ----------------

class NxFactory : public IClassFactory {
public:
    NxFactory(const GUID* clsid) : clsid_(clsid) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --ref_;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown*, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        res::com_ptr<NxCommand> cmd(new NxCommand(clsid_));   // P2 圈禁：Release 收编
        return cmd->QueryInterface(riid, ppv);
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }

private:
    const GUID* clsid_;
    ULONG ref_ = 1;
};

// ---------------- DLL 导出 ----------------

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    const GUID* clsid = nullptr;
    if (IsEqualCLSID(rclsid, CLSID_NxMenu))
        clsid = &CLSID_NxMenu;
    else if (IsEqualCLSID(rclsid, CLSID_NxHere))
        clsid = &CLSID_NxHere;
    else if (IsEqualCLSID(rclsid, CLSID_NxInto))
        clsid = &CLSID_NxInto;
    if (clsid) {
        res::com_ptr<NxFactory> f(new NxFactory(clsid));   // P2 圈禁：Release 收编
        return f->QueryInterface(riid, ppv);
    }
    *ppv = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow() {
    return S_FALSE;
}

extern "C" BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hMod = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
