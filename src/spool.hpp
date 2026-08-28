// spool.hpp：SpoolStore（M0 版）：RAM 优先 → 临时文件溢出，随机访问读取（设计 §4）
// M1 将升级为"溢出写入与上游解压并发"；M0 为顺序拉取式（溢出 = 全量落临时文件，读端随机访问）。
#pragma once
#include "bytesource.hpp"
#include <atomic>
#include <mutex>
#include <windows.h>

namespace nx {

class SpoolBuffer : public std::enable_shared_from_this<SpoolBuffer> {
public:
    // ramCap：RAM 驻留上限；tempDir：溢出目录（空 = 系统临时目录）
    SpoolBuffer(size_t ramCap, const std::wstring& tempDir);
    ~SpoolBuffer();

    SpoolBuffer(const SpoolBuffer&) = delete;
    SpoolBuffer& operator=(const SpoolBuffer&) = delete;

    void append(std::span<const byte> p);
    void finish();
    uint64_t size() const { return total_; }
    // 随机访问读（szcom 引擎的 spool 卷视图用）
    size_t read_at(uint64_t pos, std::span<byte> buf) { return readAt(pos, buf); }
    bool overflowed() const { return overflowed_; }
    uint64_t tempBytes() const { return overflowed_ ? total_ : 0; }

    // 独立随机访问读视图；持有 SpoolBuffer 引用，最后一个释放时清理临时文件
    class Reader : public ByteSource {
    public:
        explicit Reader(std::shared_ptr<SpoolBuffer> s) : s_(std::move(s)) {}
        size_t read(std::span<byte> buf) override;
        std::optional<uint64_t> sizeHint() const override { return s_->size(); }
        void seek(uint64_t abs);
        uint64_t pos() const { return pos_; }
    private:
        std::shared_ptr<SpoolBuffer> s_;
        uint64_t pos_ = 0;
    };
    std::shared_ptr<Reader> reader() { return std::make_shared<Reader>(shared_from_this()); }

    // 子窗口视图：[start, start+len) 的独立顺序流（条目级分片组暂存用）
    class Window : public ByteSource {
    public:
        Window(std::shared_ptr<SpoolBuffer> s, uint64_t start, uint64_t len)
            : s_(std::move(s)), start_(start), len_(len) {
            if (start + len > s_->size()) throw Error("spool 窗口越界");
        }
        size_t read(std::span<byte> buf) override {
            if (pos_ >= len_) return 0;
            uint64_t avail = len_ - pos_;
            size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), avail));
            size_t got = s_->readAt(start_ + pos_, std::span<byte>(buf.data(), n));
            pos_ += got;
            return got;
        }
        std::optional<uint64_t> sizeHint() const override { return len_; }
    private:
        std::shared_ptr<SpoolBuffer> s_;
        uint64_t start_, len_, pos_ = 0;
    };

private:
    friend class Reader;
    void flushToTemp();
    size_t readAt(uint64_t pos, std::span<byte> buf);

    size_t ramCap_;
    std::wstring tempDir_;
    std::vector<byte> ram_;
    HANDLE hf_ = INVALID_HANDLE_VALUE;
    std::wstring tmpPath_;
    uint64_t total_ = 0;
    bool overflowed_ = false;
    bool finished_ = false;
    std::mutex ioM_;                // 串行化文件句柄访问
};

} // namespace nx
