#include "szcom.hpp"
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <vector>

#pragma comment(lib, "OleAut32.lib")

namespace nx::sz {

// ================= 7-Zip SDK COM 接口（vtable 顺序与官方 SDK 一致） =================

using U32 = unsigned int;
using U64 = unsigned __int64;
using I64 = __int64;
using I32 = int;

struct Z7_ISequentialInStream : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Read(void* data, U32 size, U32* processedSize) = 0;
};
struct Z7_ISequentialOutStream : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Write(const void* data, U32 size, U32* processedSize) = 0;
};
struct Z7_IInStream : Z7_ISequentialInStream {
    virtual HRESULT STDMETHODCALLTYPE Seek(I64 offset, U32 seekOrigin, U64* newPosition) = 0;
};
struct Z7_IStreamGetSize : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetSize(U64* size) = 0;
};
struct Z7_IArchiveOpenCallback : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetTotal(const U64* files, const U64* bytes) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCompleted(const U64* files, const U64* bytes) = 0;
};
struct Z7_ICryptoGetTextPassword : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* password) = 0;
};
struct Z7_IArchiveOpenVolumeCallback : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetProperty(U32 propID, PROPVARIANT* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStream(const wchar_t* name, Z7_IInStream** inStream) = 0;
};
struct Z7_IProgress : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetTotal(U64 total) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCompleted(const U64* completeValue) = 0;
};
struct Z7_IArchiveExtractCallback : Z7_IProgress {
    virtual HRESULT STDMETHODCALLTYPE GetStream(U32 index, Z7_ISequentialOutStream** outStream,
                                                I32 askExtractMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE PrepareOperation(I32 askExtractMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOperationResult(I32 operationResult) = 0;
};
struct Z7_IInArchive : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Open(Z7_IInStream* stream, const U64* maxCheckStartPosition,
                                           Z7_IArchiveOpenCallback* openCallback) = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfItems(U32* numItems) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty(U32 index, U32 propID, PROPVARIANT* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE Extract(const U32* indices, U32 numItems, I32 testMode,
                                              Z7_IArchiveExtractCallback* extractCallback) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetArchiveProperty(U32 propID, PROPVARIANT* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfProperties(U32* numProps) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyInfo(U32 index, BSTR* name, U32* propID,
                                                      VARTYPE* varType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfArchiveProperties(U32* numProps) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetArchivePropertyInfo(U32 index, BSTR* name, U32* propID,
                                                             VARTYPE* varType) = 0;
};

// GUID（依 SDK Guid.txt）
constexpr GUID kClsid7z   = {0x23170F69, 0x40C1, 0x278A, {0x10, 0x00, 0x00, 0x01, 0x10, 0x07, 0x00, 0x00}};
constexpr GUID kClsidRar  = {0x23170F69, 0x40C1, 0x278A, {0x10, 0x00, 0x00, 0x01, 0x10, 0x03, 0x00, 0x00}};
constexpr GUID kClsidRar5 = {0x23170F69, 0x40C1, 0x278A, {0x10, 0x00, 0x00, 0x01, 0x10, 0xCC, 0x00, 0x00}};
constexpr GUID kIidIArchiveOpenCallback      = {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00}};
constexpr GUID kIidICryptoGetTextPassword    = {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x05, 0x00, 0x10, 0x00, 0x00}};
constexpr GUID kIidIArchiveOpenVolumeCallback= {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x06, 0x00, 0x30, 0x00, 0x00}};
constexpr GUID kIidIInArchive                = {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x06, 0x00, 0x60, 0x00, 0x00}};
constexpr GUID kIidIArchiveExtractCallback   = {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x06, 0x00, 0x20, 0x00, 0x00}};
constexpr GUID kIidISequentialInStream       = {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00}};
constexpr GUID kIidIInStream                 = {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00}};
constexpr GUID kIidIStreamGetSize            = {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x03, 0x00, 0x06, 0x00, 0x00}};
constexpr GUID kIidISequentialOutStream      = {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00}};

// PropID（SDK PropID.h 枚举实值：IsDir=6、Size=7、Encrypted=15）
constexpr U32 kpidPath = 3, kpidName = 4, kpidIsDir = 6, kpidSize = 7, kpidEncrypted = 15;
// NOperationResult（SDK IArchive.h 枚举实值）
constexpr I32 kOpOK = 0, kOpUnsupportedMethod = 1, kOpDataError = 2, kOpCRCError = 3,
              kOpUnexpectedEnd = 5, kOpIsNotArc = 7, kOpHeadersError = 8,
              kOpWrongPassword = 9;
// NAskMode
constexpr I32 kAskSkip = 2;

// ================= DLL 加载 =================

namespace {
HMODULE g_dll = nullptr;
// 现代 7z.dll 导出 CreateObject(clsid, iid, out)（7-Zip 9.x+ 第三方标准入口）；
// 极旧版本才导出 DllGetClassObject —— 两者都探测
typedef HRESULT(WINAPI* PCreateObject)(const GUID* clsid, const GUID* iid, void** out);
typedef HRESULT(WINAPI* PDllGetClassObject)(REFCLSID rclsid, REFIID riid, void** ppv);
PCreateObject g_createObject = nullptr;
PDllGetClassObject g_getClassObject = nullptr;
std::wstring g_loadedPath, g_error;

std::wstring try_load(const std::wstring& path) {
    HMODULE h = LoadLibraryW(path.c_str());
    if (!h) return L"";
    auto co = reinterpret_cast<PCreateObject>(GetProcAddress(h, "CreateObject"));
    auto gc = reinterpret_cast<PDllGetClassObject>(GetProcAddress(h, "DllGetClassObject"));
    if (!co && !gc) {
        FreeLibrary(h);
        return L"";
    }
    g_dll = h;
    g_createObject = co;
    g_getClassObject = gc;
    g_loadedPath = path;
    return path;
}
} // namespace

bool dll_available() {
    if (g_dll) return true;
    if (!g_error.empty()) return false;
    std::wstring r = try_load(L"7z.dll");   // 默认搜索：exe 目录、PATH
    if (r.empty()) r = try_load(L"C:\\Program Files\\7-Zip\\7z.dll");
    if (r.empty()) r = try_load(L"C:\\Program Files (x86)\\7-Zip\\7z.dll");
    if (r.empty()) {
        g_error = L"未找到 7z.dll（搜索：exe 目录、7-Zip 安装目录、PATH）";
        return false;
    }
    return true;
}

std::wstring dll_path() { return g_loadedPath; }
std::wstring dll_error() { return g_error; }

// ================= 随机访问源与 IInStream =================

namespace {

class SeekInput : public RegionSource {
public:
    virtual ~SeekInput() = default;
    virtual size_t read_at(uint64_t pos, std::span<byte> buf) = 0;
    virtual uint64_t size() const = 0;
};

class FileSeekInput : public SeekInput {
public:
    // meter：根输入计量（仅 FS 卷挂；spool 卷字节来自外层已计量流，再计即重复）
    // base：窗口起始偏移（隐写模式：把 [base, EOF) 呈现为一个完整卷）
    explicit FileSeekInput(const std::wstring& path, InputMeter* meter = nullptr,
                           uint64_t base = 0)
        : meter_(meter), base_(base) {
        h_ = CreateFileW(win_long_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h_ == INVALID_HANDLE_VALUE)
            throw Error("打开卷文件失败: " + wide_to_utf8(path));
        LARGE_INTEGER sz{};
        GetFileSizeEx(h_, &sz);
        uint64_t total = static_cast<uint64_t>(sz.QuadPart);
        if (base > total)
            throw Error("卷窗口偏移越界: " + wide_to_utf8(path));
        size_ = total - base;
    }
    ~FileSeekInput() override {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    size_t read_at(uint64_t pos, std::span<byte> buf) override {
        std::lock_guard<std::mutex> lk(m_);
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(base_ + pos);
        if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) throw Error("卷定位失败");
        size_t got = 0;
        while (got < buf.size()) {
            DWORD r = 0;
            if (!ReadFile(h_, buf.data() + got, static_cast<DWORD>(buf.size() - got), &r, nullptr) ||
                r == 0)
                break;
            got += r;
        }
        if (meter_) meter_->bytes += got;
        return got;
    }
    uint64_t size() const override { return size_; }
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
    InputMeter* meter_ = nullptr;
    uint64_t base_ = 0;
    std::mutex m_;
};

class SpoolSeekInput : public SeekInput {
public:
    SpoolSeekInput(std::shared_ptr<SpoolBuffer> s, uint64_t start, uint64_t len)
        : spool_(std::move(s)), start_(start), len_(len) {}
    size_t read_at(uint64_t pos, std::span<byte> buf) override {
        if (pos >= len_) return 0;
        uint64_t avail = len_ - pos;
        size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), avail));
        return spool_->read_at(start_ + pos, std::span<byte>(buf.data(), n));
    }
    uint64_t size() const override { return len_; }
private:
    std::shared_ptr<SpoolBuffer> spool_;
    uint64_t start_, len_;
};

// 父视图区间适配（嵌套容器免 spool 直读）：转发到 RegionSource（区间本身已窗口化）
class RegionSeekInput : public SeekInput {
public:
    explicit RegionSeekInput(std::shared_ptr<RegionSource> s) : s_(std::move(s)) {}
    size_t read_at(uint64_t pos, std::span<byte> buf) override {
        return s_->read_at(pos, buf);
    }
    uint64_t size() const override { return s_->size(); }
private:
    std::shared_ptr<RegionSource> s_;
};

std::shared_ptr<SeekInput> volume_input(const VolumeSource& v, InputMeter* meter) {
    if (v.region) return std::make_shared<RegionSeekInput>(v.region);
    if (!v.fsPath.empty()) return std::make_shared<FileSeekInput>(v.fsPath, meter, v.fsBase);
    return std::make_shared<SpoolSeekInput>(v.spool, v.winStart, v.winLen);
}

std::wstring lower_w(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// IInStream（含可选 IStreamGetSize）
class InStreamImpl : public Z7_IInStream, public Z7_IStreamGetSize {
public:
    explicit InStreamImpl(std::shared_ptr<SeekInput> in) : in_(std::move(in)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, kIidISequentialInStream) ||
            IsEqualGUID(riid, kIidIInStream)) {
            *ppv = static_cast<Z7_IInStream*>(this);
        } else if (IsEqualGUID(riid, kIidIStreamGetSize)) {
            *ppv = static_cast<Z7_IStreamGetSize*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = --ref_;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE Read(void* data, U32 size, U32* processed) override {
        try {
            size_t n = in_->read_at(pos_, std::span<byte>(static_cast<byte*>(data), size));
            pos_ += n;
            if (processed) *processed = static_cast<U32>(n);
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE Seek(I64 offset, U32 origin, U64* newPos) override {
        uint64_t abs = pos_;
        if (origin == 0) abs = static_cast<uint64_t>(offset);
        else if (origin == 1) abs = pos_ + static_cast<uint64_t>(offset);
        else if (origin == 2) abs = in_->size() + static_cast<uint64_t>(offset);
        else return STG_E_INVALIDFUNCTION;
        pos_ = abs;
        if (newPos) *newPos = abs;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSize(U64* size) override {
        if (size) *size = in_->size();
        return S_OK;
    }
private:
    std::shared_ptr<SeekInput> in_;
    uint64_t pos_ = 0;
    std::atomic<ULONG> ref_{1};
};

// ================= 打开回调（进度 + 密码 + RAR 卷解析） =================

struct SharedOpenState {
    std::map<std::wstring, VolumeSource> volumes;   // 键已小写化
    std::wstring firstVolName;
    std::optional<SecureStr> password;
    bool passwordAsked = false;
    InputMeter* meter = nullptr;   // FS 卷计量（EngineOptions 透传；spool 卷不挂）
};

class OpenCb : public Z7_IArchiveOpenCallback,
               public Z7_ICryptoGetTextPassword,
               public Z7_IArchiveOpenVolumeCallback {
public:
    explicit OpenCb(std::shared_ptr<SharedOpenState> st) : st_(std::move(st)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, kIidIArchiveOpenCallback)) {
            *ppv = static_cast<Z7_IArchiveOpenCallback*>(this);
        } else if (IsEqualGUID(riid, kIidICryptoGetTextPassword)) {
            *ppv = static_cast<Z7_ICryptoGetTextPassword*>(this);
        } else if (IsEqualGUID(riid, kIidIArchiveOpenVolumeCallback)) {
            *ppv = static_cast<Z7_IArchiveOpenVolumeCallback*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = --ref_;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(const U64*, const U64*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCompleted(const U64*, const U64*) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* password) override {
        if (!password) return E_POINTER;
        *password = nullptr;
        st_->passwordAsked = true;
        if (!st_->password || st_->password->empty())
            return E_FAIL;   // 无密码 → Open 失败 → 引擎换候选
        std::wstring w = utf8_to_wide(std::string(st_->password->view()));
        *password = SysAllocStringLen(w.c_str(), static_cast<UINT>(w.size()));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(U32 propID, PROPVARIANT* value) override {
        if (!value) return E_POINTER;
        PropVariantInit(value);
        if (propID == kpidName && !st_->firstVolName.empty()) {
            value->vt = VT_BSTR;
            value->bstrVal =
                SysAllocStringLen(st_->firstVolName.c_str(), static_cast<UINT>(st_->firstVolName.size()));
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStream(const wchar_t* name, Z7_IInStream** inStream) override {
        if (!name || !inStream) return E_POINTER;
        *inStream = nullptr;
        auto it = st_->volumes.find(lower_w(name));
        if (it == st_->volumes.end()) {
            std::wstring base(name);
            size_t p = base.find_last_of(L"\\/");
            if (p != std::wstring::npos)
                it = st_->volumes.find(lower_w(base.substr(p + 1)));
            if (it == st_->volumes.end()) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }
        try {
            *inStream = new InStreamImpl(volume_input(it->second, st_->meter));
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }
private:
    std::shared_ptr<SharedOpenState> st_;
    std::atomic<ULONG> ref_{1};
};

// ================= 抽取回调（单条目 → SpoolBuffer） =================

class OutStreamImpl : public Z7_ISequentialOutStream {
public:
    explicit OutStreamImpl(SpoolBuffer* dst) : dst_(dst) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, kIidISequentialOutStream)) {
            *ppv = static_cast<Z7_ISequentialOutStream*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = --ref_;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* data, U32 size, U32* processed) override {
        try {
            dst_->append(std::span<const byte>(static_cast<const byte*>(data), size));
            if (processed) *processed = size;
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }
private:
    SpoolBuffer* dst_;
    std::atomic<ULONG> ref_{1};
};

class ExtractCb : public Z7_IArchiveExtractCallback,
                  public Z7_ICryptoGetTextPassword {
public:
    // 批量抽取：targets = 本批 {index → spool}（单条目即一项）。GetStream 按 index
    // 分发；SetOperationResult 紧跟对应文件写流结束，结果记入 results_。
    // （solid 归档逐条目单独 Extract 会从 solid 块头重解码到目标位置——O(N²)，
    // 3692 文件 2.3GB 实测 10 小时级；一次 Extract 携带一批索引则前缀每批只解一次）
    ExtractCb(std::map<uint32_t, SpoolBuffer*> targets,
              std::shared_ptr<SharedOpenState> st)
        : targets_(std::move(targets)), st_(std::move(st)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, kIidIArchiveExtractCallback)) {
            *ppv = static_cast<Z7_IArchiveExtractCallback*>(this);
        } else if (IsEqualGUID(riid, kIidICryptoGetTextPassword)) {
            *ppv = static_cast<Z7_ICryptoGetTextPassword*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = --ref_;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(U64) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCompleted(const U64*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetStream(U32 index, Z7_ISequentialOutStream** outStream,
                                        I32 askExtractMode) override {
        if (!outStream) return E_POINTER;
        *outStream = nullptr;
        lastIdx_ = index;
        if (askExtractMode == kAskSkip) return S_OK;
        auto it = targets_.find(index);
        if (it == targets_.end()) return S_OK;   // 非本批目标 → 不接流（丢弃）
        *outStream = new OutStreamImpl(it->second);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PrepareOperation(I32) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetOperationResult(I32 opResult) override {
        if (targets_.count(lastIdx_)) results_[lastIdx_] = opResult;
        return S_OK;
    }
    // 内容加密条目：抽取时经此接口取密码（7z/rar 均然）
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* password) override {
        if (!password) return E_POINTER;
        *password = nullptr;
        st_->passwordAsked = true;
        if (!st_->password || st_->password->empty()) return E_FAIL;
        std::wstring w = utf8_to_wide(std::string(st_->password->view()));
        *password = SysAllocStringLen(w.c_str(), static_cast<UINT>(w.size()));
        return S_OK;
    }
    // 每条目操作结果（kOpOK=成功；未出现的条目=未处理/被跳过）
    const std::map<uint32_t, I32>& results() const { return results_; }
private:
    std::map<uint32_t, SpoolBuffer*> targets_;
    std::map<uint32_t, I32> results_;
    uint32_t lastIdx_ = 0;
    std::shared_ptr<SharedOpenState> st_;
    std::atomic<ULONG> ref_{1};
};

// ================= SevenZipReader =================

class EntrySource;

class SevenZipReader : public ContainerReader,
                       public std::enable_shared_from_this<SevenZipReader> {
public:
    SevenZipReader(Format fmt, std::map<std::wstring, VolumeSource> volumes,
                   const std::wstring& firstVol, const LayerId& layer,
                   PasswordProvider& pw, const EngineOptions& opt);
    ~SevenZipReader() override;

    bool next(ContainerEntry& out) override;

    size_t readEntry(uint32_t idx, std::span<byte> buf);
    uint64_t entrySize(uint32_t idx) const { return items_[idx].size; }

private:
    struct Item {
        std::wstring path;
        uint64_t size = UINT64_MAX;
        bool isDir = false;
        bool encrypted = false;
    };
    friend class EntrySource;

    // 单次打开尝试；返回 false 时 failIsPassword_ 说明是否密码相关
    bool tryOpen(const SecureStr* pw);
    void closeArc();
    void enumerateItems();
    void extractOne(uint32_t idx, SpoolBuffer* dst);   // 抛 PasswordExhausted/CorruptError
    // 批量物化（调用方持 mx_）：一次 Extract 抽 [start,…) 的数据条目（字节预算
    // 截断）到各自 spool 并入缓存；批失败丢弃整批、仅单条重试被请求条目
    // （隔离坏点，其后条目触发从自身开始的新批——keepGoing 语义不变）
    void materializeBatch(uint32_t start);
    uint64_t batchBudget() const;

    Format fmt_;
    std::map<std::wstring, VolumeSource> volumes_;   // 键小写
    std::wstring firstVol_;
    LayerId layer_;                    // key = 缓存/游标键，display = 提示（批次 2）
    PasswordProvider& pw_;
    EngineOptions opt_;

    Z7_IInArchive* arc_ = nullptr;
    std::shared_ptr<SharedOpenState> openState_;
    std::shared_ptr<InStreamImpl> mainStream_;
    std::vector<Item> items_;
    uint32_t cursor_ = 0;
    bool anyEncrypted_ = false;
    std::string failMsg_;
    bool failIsPassword_ = false;
    bool triedNoPw_ = false;

    struct Cached {
        std::shared_ptr<SpoolBuffer> spool;
        uint64_t pos = 0;
    };
    std::map<uint32_t, Cached> cache_;
    // Sink 写出线程池并发调 EntrySource::read → readEntry：Extract 与 cache_
    // （及 7z.dll IInArchive/InStreamImpl 单线程约定）必须整体串行
    std::mutex mx_;
};

class EntrySource : public ByteSource {
public:
    // 批次 4（所有权模型 fixed 变体）：对读取器只持弱引用——异步写出的存活由
    // Sink 任务经 keepAlive() 令牌配套保活
    EntrySource(std::shared_ptr<SevenZipReader> r, EntryToken token)
        : r_(r), token_(token) {}
    size_t read(std::span<byte> buf) override {
        auto r = r_.lock();
        if (!r) throw Error("条目流已失效（读取器已销毁）");
        return r->readEntry(static_cast<uint32_t>(token_.seq), buf);
    }
    std::optional<uint64_t> sizeHint() const override {
        auto r = r_.lock();
        if (!r) return {};
        uint64_t s = r->entrySize(static_cast<uint32_t>(token_.seq));
        return s == UINT64_MAX ? std::optional<uint64_t>{} : s;
    }
    std::shared_ptr<void> keepAlive() const override {
        auto r = r_.lock();
        return r ? std::shared_ptr<void>(r) : nullptr;   // 别名构造令牌
    }
private:
    std::weak_ptr<SevenZipReader> r_;
    EntryToken token_;
};

SevenZipReader::SevenZipReader(Format fmt, std::map<std::wstring, VolumeSource> volumes,
                               const std::wstring& firstVol, const LayerId& layer,
                               PasswordProvider& pw, const EngineOptions& opt)
    : fmt_(fmt), volumes_(std::move(volumes)), firstVol_(firstVol), layer_(layer),
      pw_(pw), opt_(opt) {
    if (!dll_available()) throw Error(wide_to_utf8(dll_error()));
    // 卷键规范化（大小写不敏感匹配）
    {
        std::map<std::wstring, VolumeSource> norm;
        for (auto& [k, v] : volumes_) norm[lower_w(k)] = std::move(v);
        volumes_ = std::move(norm);
    }

    // 候选迭代（§6.2）：先无密码直开（未加密零成本），失败按 缓存→上次成功→候选→交互
    for (;;) {
        const SecureStr* cand = nullptr;
        SecureStr held;
        if (!triedNoPw_) {
            triedNoPw_ = true;   // 第一轮：无密码
        } else {
            auto a = pw_.nextAttempt(layer_);
            if (!a)
                throw PasswordExhausted(layer_.display,
                                        "密码缺失或已耗尽: " + layer_.display);
            held = std::move(*a);
            cand = &held;
        }
        if (!tryOpen(cand)) {
            if (failIsPassword_) continue;   // 密码错 → 下一候选
            throw CorruptError(failMsg_);
        }
        enumerateItems();
        // 探测首个含数据条目（验证密码/完整性；spool 留存为首条目缓存，不重复抽取）
        uint32_t probe = UINT32_MAX;
        for (uint32_t i = 0; i < items_.size(); ++i) {
            if (!items_[i].isDir && items_[i].size != 0 && items_[i].size != UINT64_MAX) {
                probe = i;
                break;
            }
        }
        if (probe != UINT32_MAX) {
            try {
                auto spool = std::make_shared<SpoolBuffer>(opt_.spoolRam, opt_.tempDir);
                extractOne(probe, spool.get());
                auto& slot = cache_[probe];
                slot.spool = std::move(spool);
            } catch (PasswordExhausted&) {
                closeArc();
                cache_.clear();
                if (cand) continue;   // 密码错 → 下一候选（无密码轮也落到候选环）
                continue;
            } catch (CorruptError&) {
                closeArc();
                cache_.clear();
                throw;   // 非密码损坏：报损坏
            }
        }
        if (cand) pw_.reportSuccess(layer_, *cand);
        break;
    }
}

SevenZipReader::~SevenZipReader() { closeArc(); }

void SevenZipReader::closeArc() {
    if (arc_) {
        arc_->Close();
        arc_->Release();
        arc_ = nullptr;
    }
}

bool SevenZipReader::tryOpen(const SecureStr* pw) {
    failIsPassword_ = false;
    failMsg_.clear();
    openState_ = std::make_shared<SharedOpenState>();
    openState_->volumes = volumes_;
    openState_->firstVolName = firstVol_;
    openState_->meter = opt_.meter;
    if (pw) openState_->password = SecureStr(pw->view());

    // rar：两代处理器都试（探测已知代次则顺序优先）
    GUID clsids[2] = {kClsidRar5, kClsidRar};
    int nCls = 2;
    if (fmt_ == Format::SevenZip) {
        clsids[0] = kClsid7z;
        nCls = 1;
    }
    auto mainIt = firstVol_.empty() ? volumes_.begin() : volumes_.find(lower_w(firstVol_));
    if (mainIt == volumes_.end() && !volumes_.empty()) mainIt = volumes_.begin();
    if (mainIt == volumes_.end()) {
        failMsg_ = "无卷数据";
        return false;
    }
    std::shared_ptr<SeekInput> mainInput;
    try {
        mainInput = volume_input(mainIt->second, opt_.meter);
    } catch (Error& e) {
        failMsg_ = e.what();
        return false;
    }

    for (int ci = 0; ci < nCls; ++ci) {
        Z7_IInArchive* arc = nullptr;
        HRESULT hr = E_FAIL;
        if (g_createObject) {
            hr = g_createObject(&clsids[ci], &kIidIInArchive, reinterpret_cast<void**>(&arc));
        } else if (g_getClassObject) {
            IClassFactory* cf = nullptr;
            hr = g_getClassObject(clsids[ci], IID_IClassFactory, reinterpret_cast<void**>(&cf));
            if (SUCCEEDED(hr) && cf) {
                hr = cf->CreateInstance(nullptr, kIidIInArchive, reinterpret_cast<void**>(&arc));
                cf->Release();
            }
        }
        if (FAILED(hr) || !arc) {
            failMsg_ = "7z.dll 创建处理器失败 (hr=" + std::to_string(static_cast<long>(hr)) + ")";
            continue;
        }
        auto* cb = new OpenCb(openState_);
        mainStream_ = std::make_shared<InStreamImpl>(mainInput);
        hr = arc->Open(mainStream_.get(), nullptr, cb);
        cb->Release();
        if (hr == S_OK) {
            arc_ = arc;
            return true;
        }
        arc->Release();
        mainStream_.reset();
        failMsg_ = "归档打开失败 (hr=" + std::to_string(static_cast<long>(hr)) + ")";
        failIsPassword_ = openState_->passwordAsked;
        if (failIsPassword_) return false;   // 密码错：不再试另一代处理器
    }
    return false;
}

void SevenZipReader::enumerateItems() {
    U32 n = 0;
    if (FAILED(arc_->GetNumberOfItems(&n)) || n > 100000000u)
        throw CorruptError("条目数量异常");
    items_.clear();
    anyEncrypted_ = false;
    for (U32 i = 0; i < n; ++i) {
        Item it;
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(arc_->GetProperty(i, kpidPath, &v)) && v.vt == VT_BSTR && v.bstrVal) {
            it.path = v.bstrVal;
        } else {
            PropVariantClear(&v);
            if (SUCCEEDED(arc_->GetProperty(i, kpidName, &v)) && v.vt == VT_BSTR && v.bstrVal)
                it.path = v.bstrVal;
            else
                it.path = L"(unnamed)";
        }
        PropVariantClear(&v);
        if (SUCCEEDED(arc_->GetProperty(i, kpidIsDir, &v)) && v.vt == VT_BOOL)
            it.isDir = v.boolVal != VARIANT_FALSE;
        PropVariantClear(&v);
        if (SUCCEEDED(arc_->GetProperty(i, kpidSize, &v))) {
            if (v.vt == VT_UI8) it.size = v.uhVal.QuadPart;
            else if (v.vt == VT_UI4) it.size = v.ulVal;
        }
        PropVariantClear(&v);
        if (SUCCEEDED(arc_->GetProperty(i, kpidEncrypted, &v)) && v.vt == VT_BOOL)
            it.encrypted = v.boolVal != VARIANT_FALSE;
        PropVariantClear(&v);
        if (it.encrypted) anyEncrypted_ = true;
        items_.push_back(std::move(it));
    }
}

void SevenZipReader::extractOne(uint32_t idx, SpoolBuffer* dst) {
    std::map<uint32_t, SpoolBuffer*> targets{{idx, dst}};
    ExtractCb cb(std::move(targets), openState_);
    HRESULT hr = arc_->Extract(&idx, 1, 0, &cb);
    dst->finish();
    const auto& res = cb.results();
    auto it = res.find(idx);
    I32 r = (hr == S_OK && it != res.end()) ? it->second : kOpDataError;
    if (r == kOpOK) return;
    if (r == kOpWrongPassword || (anyEncrypted_ && (r == kOpCRCError || r == kOpDataError)))
        throw PasswordExhausted(layer_.display,
                                "条目密码错误 (op=" + std::to_string(r) + ")");
    throw CorruptError("条目数据损坏 (op=" + std::to_string(r) + ")");
}

uint64_t SevenZipReader::batchBudget() const {
    // 预算 = spoolRam/2，钳 [64MiB, 1GiB]：太小 → 批多（solid 前缀重解码次数多）；
    // 太大 → 预取的未消费条目 spool 驻留内存多（Sink 异步写出滞后时）
    uint64_t b = static_cast<uint64_t>(opt_.spoolRam) / 2;
    if (b < (64ull << 20)) b = 64ull << 20;
    if (b > (1ull << 30)) b = 1ull << 30;
    return b;
}

void SevenZipReader::materializeBatch(uint32_t start) {
    constexpr size_t kMaxEntriesPerBatch = 4096;   // 防巨量小文件一批（索引/回调开销）
    const uint64_t budget = batchBudget();
    std::vector<uint32_t> idxs;
    std::map<uint32_t, SpoolBuffer*> targets;
    std::vector<std::shared_ptr<SpoolBuffer>> hold;   // 持有各条目 spool 生命周期
    uint64_t bytes = 0;
    for (uint32_t i = start; i < items_.size(); ++i) {
        if (items_[i].isDir) continue;
        uint64_t sz = items_[i].size == UINT64_MAX ? 0 : items_[i].size;
        if (!idxs.empty() && bytes + sz > budget) break;   // 首条目必入批（单个可超预算）
        auto sp = std::make_shared<SpoolBuffer>(opt_.spoolRam, opt_.tempDir);
        idxs.push_back(i);
        targets.emplace(i, sp.get());
        hold.push_back(std::move(sp));
        bytes += sz;
        if (idxs.size() >= kMaxEntriesPerBatch) break;
    }
    if (idxs.empty()) return;   // start 为目录：不缓存，readEntry 返回 0

    ExtractCb cb(std::move(targets), openState_);
    HRESULT hr = arc_->Extract(idxs.data(), static_cast<U32>(idxs.size()), 0, &cb);
    bool allOk = (hr == S_OK);
    if (allOk) {
        for (uint32_t i : idxs) {
            auto it = cb.results().find(i);
            if (it == cb.results().end() || it->second != kOpOK) {
                allOk = false;
                break;
            }
        }
    }
    if (allOk) {
        for (size_t k = 0; k < idxs.size(); ++k) {
            hold[k]->finish();
            cache_.emplace(idxs[k], Cached{std::move(hold[k]), 0});
        }
        return;
    }
    // 批内失败（某条目坏/密码）：丢弃整批——部分完成的 spool 不可信；
    // 仅单条重试被请求条目（失败按原语义抛），其后条目触发从自身开始的
    // 新批（不含坏点），keepGoing 语义与逐条目时代一致
    if (!items_[start].isDir) {
        auto sp = std::make_shared<SpoolBuffer>(opt_.spoolRam, opt_.tempDir);
        extractOne(start, sp.get());
        cache_.emplace(start, Cached{std::move(sp), 0});
    }
}

bool SevenZipReader::next(ContainerEntry& out) {
    if (cursor_ >= items_.size()) return false;
    const Item& it = items_[cursor_];
    out.name = wide_to_utf8(it.path);
    for (auto& c : out.name)
        if (c == '\\') c = '/';   // 7z.dll(Windows) kpidPath 用反斜杠，统一为 '/'
    out.isDir = it.isDir;
    out.isSymlink = false;
    out.independentData = true;   // 每条目独立 spool 支撑 → 可异步写出（D4）
    out.size = it.size;
    out.data = std::make_shared<EntrySource>(shared_from_this(),
                                   EntryToken{cursor_});
    ++cursor_;
    return true;
}

size_t SevenZipReader::readEntry(uint32_t idx, std::span<byte> buf) {
    if (idx >= items_.size()) throw Error("条目索引越界");
    std::lock_guard<std::mutex> lk(mx_);
    auto found = cache_.find(idx);
    if (found == cache_.end()) {
        materializeBatch(idx);
        found = cache_.find(idx);
        if (found == cache_.end()) return 0;   // 目录等无数据条目
    }
    Cached& c = found->second;
    if (c.pos >= c.spool->size()) return 0;
    size_t n = c.spool->read_at(c.pos, buf);
    c.pos += n;
    return n;
}

} // namespace

std::shared_ptr<ContainerReader> open_archive(Format fmt,
                                              const std::map<std::wstring, VolumeSource>& volumes,
                                              const std::wstring& firstVol,
                                              const LayerId& layer,
                                              PasswordProvider& pw,
                                              const EngineOptions& opt) {
    return std::make_shared<SevenZipReader>(fmt, volumes, firstVol, layer, pw, opt);
}

} // namespace nx::sz
