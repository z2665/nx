#include "password.hpp"
#include "gui.hpp"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>

namespace nx {

void PasswordProvider::loadPasswordFile(const std::wstring& path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) throw Error("打不开密码文件: " + wide_to_utf8(path));
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t b = 0;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
        if (b >= line.size()) continue;
        addCandidate(std::string_view(line).substr(b));
        SecureZeroMemory(line.data(), line.size());
        line.clear();
    }
}

// 询问可用性：--no-prompt 关闭；否则总有路径——控制台可用走控制台，否则 GUI 弹窗
// （M3：右键/资源管理器启动无控制台 → GUI；--gui 强制 GUI）
bool PasswordProvider::promptAvailable() {
    return !noPrompt_;
}

std::optional<SecureStr> PasswordProvider::promptInteractive(const LayerId& layer) {
    // 测试注入的脚本化提示：优先于控制台/GUI 路径
    {
        std::lock_guard<std::mutex> lk(m_);
        if (promptSink_) {
            auto s = promptSink_(layer);
            ++prompts_;
            if (!s || s->empty()) return std::nullopt;
            return SecureStr(*s);
        }
    }
    // GUI 优先：--gui 或无控制台（资源管理器右键启动）→ 弹窗；
    // 每个需要密码的层各弹一窗（§6.2 顺序链的 GUI 形态）；X/取消 → Cancelled 整体退出
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    bool consoleOk = h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != 0;
    if (guiPrompt_ || !consoleOk) {
        auto pw = gui::ask_password(layer.display + " 的密码：");
        if (!pw)
            throw Cancelled("用户取消了密码输入（" + layer.display + "）");
        std::wstring wiped = *pw;   // 尽力擦除
        SecureStr out(wide_to_utf8(*pw));
        SecureZeroMemory(wiped.data(), wiped.size() * sizeof(wchar_t));
        ++prompts_;
        return out;
    }
    DWORD oldMode = 0;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &oldMode)) return std::nullopt;
    // 回显关闭（§6.2 第 4 步）
    SetConsoleMode(h, oldMode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
    std::printf("%s 的密码：", layer.display.c_str());
    std::fflush(stdout);
    wchar_t wbuf[1024];
    DWORD n = 0;
    BOOL ok = ReadConsoleW(h, wbuf, 1024, &n, nullptr);
    SetConsoleMode(h, oldMode);
    std::printf("\n");
    if (!ok || n == 0) return std::nullopt;
    while (n > 0 && (wbuf[n - 1] == L'\r' || wbuf[n - 1] == L'\n')) --n;
    if (n == 0) return std::nullopt;
    SecureStr pw(wide_to_utf8(std::wstring(wbuf, n)));
    SecureZeroMemory(wbuf, sizeof(wchar_t) * n);
    ++prompts_;
    return pw;
}

void PasswordProvider::setPromptSink(PromptSink sink) {
    std::lock_guard<std::mutex> lk(m_);
    promptSink_ = std::move(sink);
}

// 解析链（§6.2）：缓存 → 上次成功 → 候选列表 → 交互
// 游标 = 该层已尝试次数（键 = LayerId::key）；reportSuccess 会清零重新命中缓存
std::optional<SecureStr> PasswordProvider::nextAttempt(const LayerId& layer) {
    {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<SecureStr> prefix;
        auto lc = layerCache_.find(layer.key);
        if (lc != layerCache_.end())
            prefix.emplace_back(lc->second.view());
        if (lastSuccess_ &&
            !(lc != layerCache_.end() && lc->second.view() == lastSuccess_->view()))
            prefix.emplace_back(lastSuccess_->view());
        for (auto& c : candidates_)
            prefix.emplace_back(c.view());
        size_t idx = cursor_[layer.key]++;
        if (idx < prefix.size()) return std::move(prefix[idx]);
    }
    // 不持锁进入交互（阻塞在控制台）
    if (promptAvailable()) {
        if (auto pw = promptInteractive(layer); pw && !pw->empty())
            return pw;
    }
    return std::nullopt;   // 耗尽（非交互场景自动跳过询问，§6.2 第 5 步）
}

void PasswordProvider::reportSuccess(const LayerId& layer, const SecureStr& pw) {
    std::lock_guard<std::mutex> lk(m_);
    layerCache_[layer.key] = SecureStr(pw.view());
    lastSuccess_ = SecureStr(pw.view());
    cursor_[layer.key] = 0;
}

} // namespace nx
