#include "pushback.hpp"

namespace nx {

PushbackSource::PushbackSource(SourcePtr src, size_t histCap)
    : src_(std::move(src)), histCap_(histCap) {}

void PushbackSource::pull(size_t n) {
    // 上游单次 read 可能只返回一小块（如条目流按块供给），必须循环读满。
    // 已消费前缀（pendOff_）只在需要扩容时压缩一次，避免每次 read 都 memmove。
    while (!srcEof_ && pend_.size() - pendOff_ < n) {
        if (pendOff_ > 0) {   // 压缩：未消费部分前移
            pend_.erase(pend_.begin(), pend_.begin() + pendOff_);
            pendOff_ = 0;
        }
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
    // D5 零拷贝直通：无需回看且无预取残留 → 直接读上游
    if (!historyEnabled_ && pendOff_ >= pend_.size()) {
        size_t n = src_->read(buf);
        base_ += n;
        if (n == 0) srcEof_ = true;
        return n;
    }
    if (pendOff_ >= pend_.size()) pull(buf.size());
    size_t n = std::min(buf.size(), pend_.size() - pendOff_);
    if (n == 0) return 0;
    const byte* p = pend_.data() + pendOff_;
    std::memcpy(buf.data(), p, n);
    if (historyEnabled_) {   // 检测完成后不再记录（省一级 memcpy）
        hist_.insert(hist_.end(), p, p + n);
        if (hist_.size() > histCap_)
            hist_.erase(hist_.begin(), hist_.begin() + (hist_.size() - histCap_));
    }
    pendOff_ += n;
    if (pendOff_ == pend_.size()) {   // 消费尽：O(1) 重置，避免每次 read 都 memmove
        pend_.clear();
        pendOff_ = 0;
    }
    base_ += n;
    return n;
}

std::span<const byte> PushbackSource::read_direct(size_t maxN) {
    if (maxN == 0) return {};
    // 1) 先服务预取残留（视图直借，零拷贝）
    if (pendOff_ < pend_.size()) {
        size_t n = std::min(maxN, pend_.size() - pendOff_);
        auto v = std::span<const byte>(pend_.data() + pendOff_, n);
        pendOff_ += n;
        if (pendOff_ == pend_.size()) {
            pend_.clear();
            pendOff_ = 0;
        }
        base_ += n;
        return v;
    }
    // 2) 无需回看 → 上游零拷贝视图直通
    if (!historyEnabled_) {
        auto v = src_->read_direct(maxN);
        base_ += v.size();
        if (v.empty()) srcEof_ = true;
        return v;
    }
    return {};
}

std::span<const byte> PushbackSource::peek(size_t n) {
    if (pend_.size() - pendOff_ < n) pull(n - (pend_.size() - pendOff_));
    return std::span<const byte>(pend_.data() + pendOff_,
                                 std::min(n, pend_.size() - pendOff_));
}

void PushbackSource::rewindTo(uint64_t absPos) {
    if (absPos > base_) throw Error("rewind 目标在当前位置之后");
    uint64_t back = base_ - absPos;
    if (back > hist_.size())
        throw Error("回看窗口不足（需回退 " + std::to_string(back) +
                    " 字节，仅剩 " + std::to_string(hist_.size()) + "）");
    size_t k = static_cast<size_t>(back);
    // 未消费部分后移，回退字节前插（拷贝一次）
    pend_.insert(pend_.begin(), hist_.end() - k, hist_.end());
    for (size_t i = 0; i < k; ++i) hist_.pop_back();
    base_ = absPos;
}

} // namespace nx
