// pushback.hpp：回看流 PushbackSource——管线通用适配器（自 bytesource.hpp 拆出，
// 该类三模式[历史回看/零拷贝直通/区间转发]复杂度已配得上独立文件，
// bytesource.hpp 收敛回"接口 + 简单源"）
// 用途：嗅探（peek 不消费）、多成员过滤器重启、加密容器失败重启（D2 回退）。
// 回看 = RAM 环形历史（histCap 上限，超出报错），设计 §4 ByteSource 注记
#pragma once
#include "bytesource.hpp"

namespace nx {

class PushbackSource : public ByteSource {
public:
    PushbackSource(SourcePtr src, size_t histCap = 4 << 20);
    size_t read(std::span<byte> buf) override;
    std::optional<uint64_t> sizeHint() const override { return src_->sizeHint(); }

    // 不消费地查看最多 n 字节（可能不足 n = EOF）；返回视图在下次调用前有效
    std::span<const byte> peek(size_t n);
    // D5 零拷贝直通：检测/打开完成后调用——关闭历史记录，pend 空时 read 直接穿透
    // 到上游（省 hist/pend 两级 memcpy）。此后 rewind 将失败（不再需要）。
    void setHistoryEnabled(bool on) { historyEnabled_ = on; }
    std::span<const byte> read_direct(size_t maxN) override;
    // 回退 pos 到绝对位置（须 >= histStart()）
    void rewindTo(uint64_t absPos);
    uint64_t pos() const { return base_; }
    uint64_t histStart() const { return base_ - hist_.size(); }
    std::shared_ptr<RegionSource> seekRegion() const override { return src_->seekRegion(); }
    std::shared_ptr<void> keepAlive() const override { return src_->keepAlive(); }

private:
    void pull(size_t n);   // 从上游补充 pend 至少 n 字节（或 EOF）
    SourcePtr src_;
    std::vector<byte> pend_;      // 已从上游拉出、未交付给消费者
    size_t pendOff_ = 0;          // pend_ 已消费前缀（读满即重置，避免逐次 memmove）
    std::deque<byte> hist_;       // 已交付字节（回看窗口）
    size_t histCap_;
    uint64_t base_ = 0;           // pend_[0] 的绝对位置
    bool srcEof_ = false;
    bool historyEnabled_ = true;  // 检测/回退阶段后关闭（D5 直通）
};

} // namespace nx
