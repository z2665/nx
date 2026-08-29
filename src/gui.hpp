// gui.hpp：M3 GUI 弹窗 —— 密码输入 / 输出前缀输入（内存对话框模板，无资源文件）
// X 或取消 → 返回 nullopt（调用方语义：取消整个任务并退出）
#pragma once
#include <optional>
#include <string>

namespace nx::gui {

// 密码输入：title 如 "第 2 层 inner.zip 的密码"；返回输入或 nullopt（取消）
std::optional<std::wstring> ask_password(const std::string& titleUtf8);

// 前缀输入：defaultValue 预填（默认=归档文件名）；返回输入或 nullopt（取消）
std::optional<std::wstring> ask_prefix(const std::wstring& defaultValue);

// 完成提示（无控制台运行时的反馈）；cancelled 为 true 时不弹
void notify_done(bool ok, const std::string& detailUtf8);

} // namespace nx::gui
