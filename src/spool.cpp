#include "spool.hpp"
#include "diag.hpp"
#include <algorithm>
#include <cstring>

namespace nx {

SpoolBuffer::SpoolBuffer(size_t ramCap, const std::wstring& tempDir)
    : ramCap_(ramCap ? ramCap : (64 << 20)), tempDir_(tempDir) {
    diag::track_spool(this);   // S1 哨兵：活性登记
}

SpoolBuffer::~SpoolBuffer() {
    diag::untrack_spool(this);
    if (hf_ != INVALID_HANDLE_VALUE) CloseHandle(hf_);   // DELETE_ON_CLOSE：句柄关闭即删
}

void SpoolBuffer::flushToTemp() {
    if (overflowed_) return;
    tmpPath_ = make_temp_file_path(tempDir_);
    // FILE_FLAG_DELETE_ON_CLOSE：句柄一关（正常析构/异常退出/进程被杀）OS 即删——
    // 独占句柄天然满足"唯一持有者"；spool 读经同一句柄（不按路径重开），无冲突。
    // （真实案例：15GB spool 在成功运行结束后残留——对象级泄漏/强杀时 DeleteFileW
    // 永远没机会执行，DELETE_ON_CLOSE 把清理责任交给内核）
    HANDLE h = CreateFileW(tmpPath_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw Error("创建临时文件失败: " + wide_to_utf8(win32_last_error_text()));
    hf_ = h;
    overflowed_ = true;
    // RAM 全量落盘（分块 ≤16MiB：ram_ 可达 8GiB，一次 cast DWORD 会截断——
    // 真实案例 8GiB 恰为 2×4GiB，截断成 0 后 WriteFile 成功写入 0 字节，
    // 表现为"写临时文件失败: 操作成功完成 (Win32 0)"）
    size_t off = 0;
    while (off < ram_.size()) {
        size_t want = std::min<size_t>(ram_.size() - off, 16u << 20);
        DWORD wrote = 0;
        if (!WriteFile(hf_, ram_.data() + off, static_cast<DWORD>(want), &wrote, nullptr)
            || wrote == 0)
            throw Error("写临时文件失败: " + wide_to_utf8(win32_last_error_text()));
        off += wrote;
    }
    ram_.clear();
    ram_.shrink_to_fit();
}

void SpoolBuffer::append(std::span<const byte> p) {
    if (finished_) throw Error("spool 已结束，不能再写入");
    if (p.empty()) return;
    if (!overflowed_) {
        if (ram_.size() + p.size() <= ramCap_) {
            ram_.insert(ram_.end(), p.begin(), p.end());
            total_ += p.size();
            return;
        }
        flushToTemp();
    }
    // 溢出路径：直接写文件
    std::lock_guard<std::mutex> lk(ioM_);
    size_t off = 0;
    while (off < p.size()) {
        DWORD wrote = 0;
        if (!WriteFile(hf_, p.data() + off, static_cast<DWORD>(p.size() - off), &wrote, nullptr)
            || wrote == 0)
            throw Error("写临时文件失败: " + wide_to_utf8(win32_last_error_text()));
        off += wrote;
    }
    total_ += p.size();
}

void SpoolBuffer::finish() {
    finished_ = true;
}

size_t SpoolBuffer::readAt(uint64_t pos, std::span<byte> buf) {
    if (!finished_) throw Error("spool 尚未结束，不能读取");
    if (pos >= total_) return 0;
    uint64_t avail = total_ - pos;
    size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), avail));
    if (!overflowed_) {
        std::memcpy(buf.data(), ram_.data() + pos, n);
        return n;
    }
    std::lock_guard<std::mutex> lk(ioM_);
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(pos);
    if (!SetFilePointerEx(hf_, li, nullptr, FILE_BEGIN))
        throw Error("临时文件定位失败: " + wide_to_utf8(win32_last_error_text()));
    size_t got = 0;
    while (got < n) {
        DWORD r = 0;
        if (!ReadFile(hf_, buf.data() + got, static_cast<DWORD>(n - got), &r, nullptr) || r == 0)
            throw Error("读临时文件失败: " + wide_to_utf8(win32_last_error_text()));
        got += r;
    }
    return got;
}

} // namespace nx
