// password.hpp：分层密码解析（设计 §6，M0：候选列表 + 交互 + 缓存/LRU）
#pragma once
#include "util.hpp"
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nx {

// 密码安全纪律（§6.3）：固定容量缓冲 + 析构擦除；禁止 std::string 存密码
class SecureStr {
public:
    SecureStr() = default;
    explicit SecureStr(std::string_view s) {
        if (s.empty()) return;
        len_ = s.size();
        buf_ = std::make_unique<char[]>(len_ + 1);
        std::memcpy(buf_.get(), s.data(), len_);
        buf_[len_] = '\0';
    }
    ~SecureStr() { wipe(); }
    SecureStr(const SecureStr&) = delete;
    SecureStr& operator=(const SecureStr&) = delete;
    SecureStr(SecureStr&& o) noexcept : buf_(std::move(o.buf_)), len_(o.len_) { o.len_ = 0; }
    SecureStr& operator=(SecureStr&& o) noexcept {
        if (this != &o) { wipe(); buf_ = std::move(o.buf_); len_ = o.len_; o.len_ = 0; }
        return *this;
    }
    void wipe() {
        if (buf_) {
            // volatile 写循环：等价 SecureZeroMemory，防优化器消除（§6.3）
            volatile char* p = buf_.get();
            for (size_t i = 0; i < len_ + 1; ++i) p[i] = 0;
        }
        buf_.reset();
        len_ = 0;
    }
    bool empty() const { return len_ == 0; }
    const char* c_str() const { return buf_ ? buf_.get() : ""; }
    std::string_view view() const { return buf_ ? std::string_view(buf_.get(), len_) : std::string_view(); }
private:
    std::unique_ptr<char[]> buf_;
    size_t len_ = 0;
};

// 每层独立解析链：缓存 → 上次成功 → 候选列表 → 交互询问（§6.2）
class PasswordProvider {
public:
    void addCandidate(std::string_view pw) {
        std::lock_guard<std::mutex> lk(m_);
        candidates_.emplace_back(pw);
    }
    void loadPasswordFile(const std::wstring& path);
    void setNoPrompt(bool v) { noPrompt_ = v; }

    // 引擎逐次取候选（每次调用推进游标；耗尽后进入交互；再耗尽返回空）
    // layerId：层身份（逻辑路径 + 格式），用于提示与缓存
    std::optional<SecureStr> nextAttempt(const std::string& layerId);

    // 某层验证成功：写缓存 + 全局 LRU
    void reportSuccess(const std::string& layerId, const SecureStr& pw);

    // 交互提示是否可用（TTY 且未 --no-prompt）
    bool promptAvailable();

    uint64_t promptCount() const { return prompts_; }

private:
    std::optional<SecureStr> promptInteractive(const std::string& layerId);

    std::mutex m_;
    std::vector<SecureStr> candidates_;
    bool noPrompt_ = false;
    std::map<std::string, SecureStr> layerCache_;        // 层缓存
    std::optional<SecureStr> lastSuccess_;               // 全局上次成功（LRU 简化）
    std::map<std::string, size_t> cursor_;               // 每层候选游标
    std::map<std::string, bool> promptOpened_;           // 每层交互是否已开启
    uint64_t prompts_ = 0;
};

} // namespace nx
