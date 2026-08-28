// bytesource.hpp：唯一流抽象 + 各实现（设计 §4 核心抽象）
#pragma once
#include "util.hpp"
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nx {

class ByteSource {
public:
    virtual ~ByteSource() = default;
    // 顺序读：返回读取字节数，0 = EOF。阻塞。
    virtual size_t read(std::span<byte> buf) = 0;
    // 可选零拷贝读（D5）：返回内部缓冲视图，有效至下一次 read/read_direct；
    // 不支持或 EOF 返回空 span（调用方退回 read() 判 EOF）。
    virtual std::span<const byte> read_direct(size_t maxN) { (void)maxN; return {}; }
    virtual std::optional<uint64_t> sizeHint() const { return {}; }
};

using SourcePtr = std::unique_ptr<ByteSource>;

// 计量：根输入文件读取量（供压缩比熔断使用，设计 D6）
struct InputMeter {
    std::atomic<uint64_t> bytes{0};
};

// 文件（顺序读，FILE_FLAG_SEQUENTIAL_SCAN）
class FileSource : public ByteSource {
public:
    explicit FileSource(const std::wstring& path, InputMeter* meter = nullptr);
    ~FileSource() override;
    size_t read(std::span<byte> buf) override;
    std::optional<uint64_t> sizeHint() const override { return size_; }
private:
    void* handle_ = nullptr;   // HANDLE
    uint64_t size_ = 0;
    InputMeter* meter_ = nullptr;
};

// 分片虚拟拼接（cat 语义，设计 D3）
class ConcatSource : public ByteSource {
public:
    explicit ConcatSource(std::vector<std::shared_ptr<ByteSource>> parts);
    size_t read(std::span<byte> buf) override;
    std::optional<uint64_t> sizeHint() const override;
private:
    std::vector<std::shared_ptr<ByteSource>> parts_;
    size_t cur_ = 0;
    bool hintKnown_ = true;
    uint64_t hint_ = 0;
};

// 空流（probe 重放的空文件条目用）
class NullSource : public ByteSource {
public:
    size_t read(std::span<byte>) override { return 0; }
};

// 共享持有的包装（把 shared_ptr 适配成 unique 语义给 PushbackSource 用）
class SharedView : public ByteSource {
public:
    explicit SharedView(std::shared_ptr<ByteSource> inner) : inner_(std::move(inner)) {}
    size_t read(std::span<byte> buf) override { return inner_->read(buf); }
    std::span<const byte> read_direct(size_t maxN) override { return inner_->read_direct(maxN); }
    std::optional<uint64_t> sizeHint() const override { return inner_->sizeHint(); }
private:
    std::shared_ptr<ByteSource> inner_;
};

// 有界队列（定义在 pipes.hpp）
template <class T> class BoundedQueue;

// 消费队列的流（过滤器泵的下游）
class QueueSource : public ByteSource {
public:
    explicit QueueSource(BoundedQueue<std::vector<byte>>& q);
    ~QueueSource() override;
    size_t read(std::span<byte> buf) override;
private:
    BoundedQueue<std::vector<byte>>& q_;
    std::vector<byte> cur_;
    size_t off_ = 0;
    bool eof_ = false;
};

// 回看流：支持 peek / rewind（RAM 环形历史，超出上限报错；设计 §4 ByteSource 注记）
// 用途：嗅探、多成员过滤器重启、加密容器失败重启（D2 回退）
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
