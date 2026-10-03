#include "bytesource.hpp"
#include "pipes.hpp"
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace nx {

// ---------- FileSource ----------

FileSource::FileSource(const std::wstring& path, InputMeter* meter) : meter_(meter) {
    std::wstring p = win_long_path(path);
    handle_ = res::adopt_file(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                          OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!handle_.valid())
        throw Error("打开文件失败: " + wide_to_utf8(path) + " " + wide_to_utf8(win32_last_error_text()));
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(handle_.get(), &sz)) size_ = static_cast<uint64_t>(sz.QuadPart);
}

size_t FileSource::read(std::span<byte> buf) {
    size_t total = 0;
    while (total < buf.size()) {
        DWORD want = static_cast<DWORD>(std::min<size_t>(buf.size() - total, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(handle_.get(), buf.data() + total, want, &got, nullptr)) {
            DWORD e = GetLastError();
            if (e == ERROR_BROKEN_PIPE) break;
            throw Error("读文件失败: " + wide_to_utf8(win32_last_error_text()));
        }
        if (got == 0) break;
        total += got;
    }
    if (meter_) meter_->bytes.fetch_add(total, std::memory_order_relaxed);
    return total;
}

// ---------- ConcatSource ----------

ConcatSource::ConcatSource(std::vector<std::shared_ptr<ByteSource>> parts)
    : parts_(std::move(parts)) {
    for (auto& p : parts_) {
        auto h = p->sizeHint();
        if (h) hint_ += *h; else { hintKnown_ = false; break; }
    }
}

size_t ConcatSource::read(std::span<byte> buf) {
    while (cur_ < parts_.size()) {
        size_t n = parts_[cur_]->read(buf);
        if (n > 0) return n;
        ++cur_;
    }
    return 0;
}

std::optional<uint64_t> ConcatSource::sizeHint() const {
    if (hintKnown_) return hint_;
    return {};
}

// ---------- QueueSource ----------

QueueSource::QueueSource(BoundedQueue<std::vector<byte>>& q) : q_(q) {}
QueueSource::~QueueSource() { q_.abandon(); }

size_t QueueSource::read(std::span<byte> buf) {
    if (eof_) return 0;
    size_t total = 0;
    while (total < buf.size()) {
        if (off_ >= cur_.size()) {
            auto blk = q_.pop();
            if (!blk) { eof_ = true; break; }
            cur_ = std::move(*blk);
            off_ = 0;
            continue;
        }
        size_t n = std::min(buf.size() - total, cur_.size() - off_);
        std::memcpy(buf.data() + total, cur_.data() + off_, n);
        off_ += n;
        total += n;
    }
    return total;
}

// ---------- PushbackSource 在 pushback.cpp ----------

} // namespace nx
