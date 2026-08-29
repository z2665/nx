// gui.hpp：M3 GUI 弹窗 —— 密码输入 / 输出前缀输入（内存对话框模板，无资源文件）
// + 进度窗（待办 #1：GUI 模式解压的中间反馈与取消）。
// X 或取消 → 返回 nullopt（调用方语义：取消整个任务并退出）
#pragma once
#include "bytesource.hpp"
#include "session.hpp"
#include <optional>
#include <string>

namespace nx::gui {

// 密码输入：title 如 "第 2 层 inner.zip 的密码"；返回输入或 nullopt（取消）
std::optional<std::wstring> ask_password(const std::string& titleUtf8);

// 前缀输入：defaultValue 预填（默认=归档文件名）；返回输入或 nullopt（取消）
std::optional<std::wstring> ask_prefix(const std::wstring& defaultValue);

// 完成提示（无控制台运行时的反馈）；cancelled 为 true 时不弹。
// titleOverride：自定义窗口标题（如隐写"未检测到"提示），null 用默认完成/失败标题
void notify_done(bool ok, const std::string& detailUtf8,
                 const wchar_t* titleOverride = nullptr);

// ---- 进度窗（独立 GUI 线程上的无模式对话框；--gui 或 Explorer 启动时显示）----
// GUI 线程定时轮询 Stats/InputMeter 原子量（只读），取消时置 abortFlag（原子写）。
// 百分比 = meter.bytes / stats.inputTotal（根输入消耗比；直读视图已挂计量），
// 无分母或重读超出时封顶 99% 至收尾，分母未知回退动画条（设计 §10 待办 #2）。
void progress_show(const std::wstring& caption, Stats* stats, const InputMeter* meter);
void progress_hide();                              // 幂等；join GUI 线程
bool progress_cancelled();                         // 用户点了取消/X
void progress_file(const std::string& relUtf8);    // Sink::writeOne：当前写出文件
void progress_stage(const std::string& stageUtf8); // Walker：当前展开的容器/过滤器

} // namespace nx::gui
