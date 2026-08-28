#include "spool.hpp"
#include <algorithm>
#include <cstring>

namespace nx {

SpoolBuffer::SpoolBuffer(size_t ramCap, const std::wstring& tempDir)
    : ramCap_(ramCap ? ramCap : (64 << 20)), tempDir_(tempDir) {}

SpoolBuffer::~SpoolBuffer() {
    if (hf_ != INVALID_HANDLE_VALUE) CloseHandle(hf_);
    if (!tmpPath_.empty()) DeleteFileW(tmpPath_.c_str());   // 引用计数归零即删（设计 §4）
}

void SpoolBuffer::flushToTemp() {
    if (overflowed_) return;
    tmpPath_ = make_temp_file_path(tempDir_);
    HANDLE h = CreateFileW(tmpPath_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw Error("创建临时文件失败: " + wide_to_utf8(win32_last_error_text()));
    hf_ = h;
    overflowed_ = true;
    // RAM 全量落盘
    size_t off = 0;
    while (off < ram_.size()) {
        DWORD wrote = 0;
        if (!WriteFile(hf_, ram_.data() + off, static_cast<DWORD>(ram_.size() - off), &wrote, nullptr)
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

void SpoolBuffer::Reader::seek(uint64_t abs) {
    if (abs > s_->size()) throw Error("seek 越界");
    pos_ = abs;
}

size_t SpoolBuffer::Reader::read(std::span<byte> buf) {
    size_t n = s_->readAt(pos_, buf);
    pos_ += n;
    return n;
}

} // namespace nx
