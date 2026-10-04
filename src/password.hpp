// password.hpp：分层密码解析（设计 §6）
#pragma once
#include "util.hpp"
#include <functional>
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

// 层身份：key 与 display 职责分离——
//   key     = 密码缓存/游标键：容器逻辑路径（"outer.tar.gz/a.tar.gz/data.zip"），
//             兄弟分支（不同父容器下的同名同深）不再共享缓存与游标；
//   display = 交互提示与错误消息（"第 3 层 data.zip (zip)"）。
// 原实现两者混用 depth+name 字符串：a/b 两父容器各含 data.zip 分片组时，
// a 组耗尽候选会把共享 cursor 推过界，b 组连候选都不试即假性 PasswordExhausted
struct LayerId {
    std::string key;
    std::string display;
};

// 提示注入：默认走控制台/GUI（promptInteractive 内建）；
// 测试注入脚本化应答（返回 nullopt = 无输入，等价读取失败 → 进入耗尽）。
// 携带层身份（display 展示文本），可按层断言提示内容
using PromptSink = std::function<std::optional<std::string>(const LayerId&)>;

// 每层独立解析链：缓存 → 上次成功 → 候选列表 → 交互询问（§6.2）
class PasswordProvider {
public:
    void addCandidate(std::string_view pw) {
        std::lock_guard<std::mutex> lk(m_);
        candidates_.emplace_back(pw);
    }
    void loadPasswordFile(const std::wstring& path);
    void setNoPrompt(bool v) { noPrompt_ = v; }
    void setGuiPrompt(bool v) { guiPrompt_ = v; }
    void setPromptSink(PromptSink sink);   // 测试注入（优先于控制台/GUI 路径）

    // 引擎逐次取候选（每次调用推进游标；耗尽后进入交互；再耗尽返回空）
    std::optional<SecureStr> nextAttempt(const LayerId& layer);

    // 某层验证成功：写缓存 + 全局 LRU
    void reportSuccess(const LayerId& layer, const SecureStr& pw);

    // 交互提示是否可用（TTY 且未 --no-prompt）
    bool promptAvailable();

    uint64_t promptCount() const { return prompts_; }

private:
    std::optional<SecureStr> promptInteractive(const LayerId& layer);

    std::mutex m_;
    std::vector<SecureStr> candidates_;
    bool noPrompt_ = false;
    bool guiPrompt_ = false;
    PromptSink promptSink_;                                // 测试注入的脚本化提示
    std::map<std::string, SecureStr> layerCache_;        // 层缓存（键 = LayerId::key）
    std::optional<SecureStr> lastSuccess_;               // 全局上次成功（LRU 简化）
    std::map<std::string, size_t> cursor_;               // 每层候选游标（键 = LayerId::key）
    uint64_t prompts_ = 0;
};

} // namespace nx
