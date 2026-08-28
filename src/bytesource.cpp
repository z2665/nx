#include "bytesource.hpp"
#include "pipes.hpp"
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace nx {

// ---------- FileSource ----------

FileSource::FileSource(const std::wstring& path, InputMeter* meter) : meter_(meter) {
    std::wstring p = win_long_path(path);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw Error("打开文件失败: " + wide_to_utf8(path) + " " + wide_to_utf8(win32_last_error_text()));
    handle_ = h;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(h, &sz)) size_ = static_cast<uint64_t>(sz.QuadPart);
}

FileSource::~FileSource() {
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

size_t FileSource::read(std::span<byte> buf) {
    size_t total = 0;
    while (total < buf.size()) {
        DWORD want = static_cast<DWORD>(std::min<size_t>(buf.size() - total, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), buf.data() + total, want, &got, nullptr)) {
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

// ---------- PushbackSource ----------

PushbackSource::PushbackSource(SourcePtr src, size_t histCap)
    : src_(std::move(src)), histCap_(histCap) {}

void PushbackSource::pull(size_t n) {
    // 上游单次 read 可能只返回一小块（如条目流按块供给），必须循环读满
    while (!srcEof_ && pend_.size() < n) {
        size_t want = std::max<size_t>(n - pend_.size(), 64 << 10);
        size_t old = pend_.size();
        pend_.resize(old + want);
        size_t got = 0;
        try {
            got = src_->read(std::span<byte>(pend_.data() + old, want));
        } catch (...) {
            pend_.resize(old);
            throw;
        }
        pend_.resize(old + got);
        if (got == 0) srcEof_ = true;
    }
}

size_t PushbackSource::read(std::span<byte> buf) {
    if (buf.empty()) return 0;
    if (pend_.empty()) pull(buf.size());
    size_t n = std::min(buf.size(), pend_.size());
    if (n == 0) return 0;
    std::memcpy(buf.data(), pend_.data(), n);
    // 移入历史
    hist_.insert(hist_.end(), pend_.begin(), pend_.begin() + n);
    while (hist_.size() > histCap_) hist_.pop_front();
    pend_.erase(pend_.begin(), pend_.begin() + n);
    base_ += n;
    return n;
}

std::span<const byte> PushbackSource::peek(size_t n) {
    if (pend_.size() < n) pull(n - pend_.size());
    return std::span<const byte>(pend_.data(), std::min(n, pend_.size()));
}

void PushbackSource::rewindTo(uint64_t absPos) {
    if (absPos > base_) throw Error("rewind 目标在当前位置之后");
    uint64_t back = base_ - absPos;
    if (back > hist_.size())
        throw Error("回看窗口不足（需回退 " + std::to_string(back) +
                    " 字节，仅剩 " + std::to_string(hist_.size()) + "）");
    size_t k = static_cast<size_t>(back);
    pend_.insert(pend_.begin(), hist_.end() - k, hist_.end());
    for (size_t i = 0; i < k; ++i) hist_.pop_back();
    base_ = absPos;
}

} // namespace nx
