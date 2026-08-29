#include "gui.hpp"
#include "util.hpp"
#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "Comctl32.lib")
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace nx::gui {

namespace {

constexpr int IDC_LABEL = 1000;
constexpr int IDC_EDIT = 1001;
constexpr int IDC_PTITLE = 1002;   // 进度窗：输入文件名
constexpr int IDC_PCUR = 1003;     // 进度窗：当前活动（容器/文件）
constexpr int IDC_PBAR = 1004;     // 进度窗：动画条
constexpr int IDC_PSTATS = 1005;   // 进度窗：已输出统计

struct AskCtx {
    std::wstring label;     // 顶部提示（层身份/说明）
    std::wstring caption;   // 窗口标题
    std::wstring prefill;
    bool password = false;
};

unsigned short class_atom(const wchar_t* cls) {
    if (wcscmp(cls, L"BUTTON") == 0) return 0x0080;
    if (wcscmp(cls, L"EDIT") == 0) return 0x0081;
    return 0x0082;   // STATIC
}

// 内存 DLGTEMPLATE：提示 + 编辑框 + 确定/取消（无资源文件，M3 便携形态）
class TplBuilder {
public:
    TplBuilder() : p_(reinterpret_cast<UINT_PTR>(buf_)) {
        std::memset(buf_, 0, sizeof(buf_));
    }

    // DLGTEMPLATE 头（不含条目）：各弹窗共用
    void header(const wchar_t* caption, short cx, short cy) {
        // DLGTEMPLATE: style, dwExtendedStyle, cdit, x, y, cx, cy, menu, class, title, [font]
        put32(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT);
        put32(0);                        // dwExtendedStyle
        cditPos_ = p_;
        put16(0);                        // cdit（后补）
        put16(0); put16(0); put16(cx); put16(cy);   // x y cx cy（DLU）
        put16(0x0000);                   // menu: 空串 = 无
        put16(0x0000);                   // class: 空串 = 默认对话框类（0x0081 是 Edit 原子，非法）
        putwstr(caption);
        put16(9);                        // DS_SETFONT：先字号
        putwstr(L"Segoe UI");            // 后字体名
    }

    size_t build(const AskCtx& ctx) {
        header(ctx.caption.c_str(), 262, 116);
        unsigned long es = ES_LEFT | ES_AUTOHSCROLL | (ctx.password ? ES_PASSWORD : 0);
        item(WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 6, 8, 250, 24, L"STATIC", L"");
        item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | es, 0, 6, 36, 250, 14,
             L"EDIT", ctx.prefill.c_str(), IDC_EDIT);
        item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 108, 90, 68, 14,
             L"BUTTON", L"确定", IDOK);
        item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 184, 90, 68, 14,
             L"BUTTON", L"取消", IDCANCEL);
        return p_ - reinterpret_cast<UINT_PTR>(buf_);
    }

    const unsigned char* data() const { return buf_; }

private:
    void align() { p_ = (p_ + 3) & ~static_cast<UINT_PTR>(3); }
    void put16(unsigned short v) {
        *reinterpret_cast<unsigned short*>(p_) = v;
        p_ += 2;
    }
    void put32(unsigned long v) {
        align();
        *reinterpret_cast<unsigned long*>(p_) = v;
        p_ += 4;
    }
    void putwstr(const wchar_t* s) {
        while (*s) {
            *reinterpret_cast<wchar_t*>(p_) = *s++;
            p_ += 2;
        }
        *reinterpret_cast<wchar_t*>(p_) = 0;
        p_ += 2;
    }
    // DLGITEMTEMPLATE: style, dwExtendedStyle, x, y, cx, cy, id, class, title, creationData
public:
    void item(unsigned long style, unsigned long exStyle, short x, short y, short cx, short cy,
              const wchar_t* cls, const wchar_t* text, int id = -1) {
        align();
        put32(style);
        put32(exStyle);
        put16(static_cast<unsigned short>(x));
        put16(static_cast<unsigned short>(y));
        put16(static_cast<unsigned short>(cx));
        put16(static_cast<unsigned short>(cy));
        put16(static_cast<unsigned short>(id));
        put16(0xFFFF);                 // 类以原子表示
        put16(class_atom(cls));
        putwstr(text);
        put16(0);                      // 无创建数据
        ++cdit_;
    }

    // 同上，但类为名字符串（如 msctls_progress32 —— 非内置原子）
    void item_cls(unsigned long style, unsigned long exStyle, short x, short y, short cx,
                  short cy, const wchar_t* cls, int id) {
        align();
        put32(style);
        put32(exStyle);
        put16(static_cast<unsigned short>(x));
        put16(static_cast<unsigned short>(y));
        put16(static_cast<unsigned short>(cx));
        put16(static_cast<unsigned short>(cy));
        put16(static_cast<unsigned short>(id));
        putwstr(cls);                  // 类名字符串（无 0xFFFF 前缀）
        putwstr(L"");                  // title
        put16(0);                      // 无创建数据
        ++cdit_;
    }

private:
    unsigned char buf_[2048];
    UINT_PTR p_;
    UINT_PTR cditPos_ = 0;
    int cdit_ = 0;

    friend size_t finalize(TplBuilder&);
};

size_t finalize(TplBuilder& b) {
    *reinterpret_cast<unsigned short*>(b.cditPos_) = static_cast<unsigned short>(b.cdit_);
    return 0;
}

struct Runtime {
    const AskCtx* ctx;
    std::wstring result;
    bool ok = false;
};

INT_PTR CALLBACK ask_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_INITDIALOG: {
            auto* rt = reinterpret_cast<Runtime*>(l);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(rt));
            const AskCtx* c = rt->ctx;
            SetDlgItemTextW(h, IDC_LABEL, c->label.c_str());
            SetFocus(GetDlgItem(h, IDC_EDIT));
            SendDlgItemMessageW(h, IDC_EDIT, EM_SETSEL, 0, -1);
            return FALSE;   // 焦点已自设
        }
        case WM_COMMAND: {
            if (LOWORD(w) == IDOK) {
                auto* rt = reinterpret_cast<Runtime*>(GetWindowLongPtrW(h, GWLP_USERDATA));
                wchar_t buf[1024];
                GetDlgItemTextW(h, IDC_EDIT, buf, 1024);
                rt->result = buf;
                rt->ok = true;
                EndDialog(h, 1);
                return TRUE;
            }
            if (LOWORD(w) == IDCANCEL) {
                EndDialog(h, 0);
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:      // X → 取消（M3 需求 5）
            EndDialog(h, 0);
            return TRUE;
        default:
            return FALSE;
    }
    return FALSE;   // WM_COMMAND break 出口
}

std::optional<std::wstring> run_input_dialog(const AskCtx& ctx) {
    Runtime rt{&ctx, {}, false};
    TplBuilder b;
    size_t sz = b.build(ctx);
    finalize(b);
    (void)sz;
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
                                        reinterpret_cast<const DLGTEMPLATE*>(b.data()), nullptr,
                                        ask_proc, reinterpret_cast<LPARAM>(&rt));
    if (r == 1 && rt.ok)
        return rt.result;
    return std::nullopt;   // 取消 / X / 关闭
}

// ---- 进度窗（待办 #1）----

struct ProgressState {
    std::mutex m;                       // 保护 hwnd/closeReq/curLine/caption/thread
    HWND hwnd = nullptr;
    bool closeReq = false;              // progress_hide 请求正常收尾
    std::atomic<bool> cancelled{false}; // 用户取消/X
    std::atomic<bool> active{false};
    Stats* stats = nullptr;             // show 后不变：GUI 线程读原子量/取消时写 abortFlag
    const InputMeter* meter = nullptr;  // show 后不变：根输入消耗（百分比分子）
    ULONGLONG t0 = 0;
    std::wstring caption;               // 顶部标题行（输入文件名）
    std::wstring curLine;               // 当前活动行
    std::unique_ptr<std::jthread> thread;
    // 故意不 join 兜底：若 main 异常路径漏调 hide，进程退出时线程随进程终止，
    // 静态析构 join 反而会卡在 DialogBox 消息循环上。
};
ProgressState g_prog;

void init_comctl_once() {
    static bool done = [] {
        INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_BAR_CLASSES};
        InitCommonControlsEx(&ic);
        return true;
    }();
    (void)done;
}

void build_progress_tpl(TplBuilder& b) {
    constexpr unsigned long SS_LINE = WS_CHILD | WS_VISIBLE | SS_LEFT;
    b.item(SS_LINE | SS_ENDELLIPSIS, 0, 6, 8, 288, 10, L"STATIC", L"", IDC_PTITLE);
    b.item(SS_LINE | SS_ENDELLIPSIS, 0, 6, 22, 288, 10, L"STATIC", L"", IDC_PCUR);
    b.item_cls(WS_CHILD | WS_VISIBLE | WS_BORDER, 0, 6, 38, 288, 12,
               L"msctls_progress32", IDC_PBAR);
    b.item(SS_LINE, 0, 6, 58, 288, 10, L"STATIC", L"", IDC_PSTATS);
    b.item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 116, 76, 68, 14,
           L"BUTTON", L"取消", IDCANCEL);
}

void progress_cancel_now(HWND h) {
    bool first = !g_prog.cancelled.exchange(true);
    if (g_prog.stats) g_prog.stats->abortFlag.store(true);
    if (first) KillTimer(h, 1);
    EndDialog(h, 0);
}

void progress_refresh(HWND h) {
    std::wstring cur;
    {
        std::lock_guard<std::mutex> lk(g_prog.m);
        cur = g_prog.curLine;
    }
    SetDlgItemTextW(h, IDC_PCUR, cur.empty() ? L"准备中…" : cur.c_str());
    const Stats* st = g_prog.stats;
    uint64_t bytes = st ? st->bytesOut.load(std::memory_order_relaxed) : 0;
    uint64_t files = st ? st->filesOut.load(std::memory_order_relaxed) : 0;
    double sec = (GetTickCount64() - g_prog.t0) / 1000.0;
    wchar_t buf[160];
    swprintf(buf, 160, L"已输出 %s · %llu 个文件 · 已用 %.1f s",
             utf8_to_wide(format_size(bytes)).c_str(),
             static_cast<unsigned long long>(files), sec);
    SetDlgItemTextW(h, IDC_PSTATS, buf);
    // 真百分比：根输入消耗比（FileSeekView/FileSeekInput 已挂计量）；
    // 重读造成的超出由 99% 封顶吸收，分母未知（0）回退三角波动画
    int pos;
    uint64_t total = st ? st->inputTotal.load(std::memory_order_relaxed) : 0;
    uint64_t done = g_prog.meter ? g_prog.meter->bytes.load(std::memory_order_relaxed) : 0;
    if (total > 0 && g_prog.meter) {
        uint64_t pct = done * 100 / total;   // 文件尺寸量级下无溢出
        pos = static_cast<int>(pct > 99 ? 99 : pct);
    } else {
        int phase = static_cast<int>((GetTickCount64() / 120) % 50);
        pos = phase < 25 ? phase * 4 : (50 - phase) * 4;
    }
    SendDlgItemMessageW(h, IDC_PBAR, PBM_SETPOS, pos, 0);
}

INT_PTR CALLBACK progress_proc(HWND h, UINT msg, WPARAM w, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG: {
            {
                std::lock_guard<std::mutex> lk(g_prog.m);
                if (g_prog.closeReq) {   // show/hide 竞态：还没露面就被收尾
                    EndDialog(h, 0);
                    return FALSE;
                }
                g_prog.hwnd = h;
            }
            SetDlgItemTextW(h, IDC_PTITLE, g_prog.caption.c_str());
            SetTimer(h, 1, 200, nullptr);
            return TRUE;
        }
        case WM_TIMER:
            progress_refresh(h);
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(w) == IDCANCEL) {
                progress_cancel_now(h);
                return TRUE;
            }
            break;
        case WM_CLOSE:   // X 等同取消（用户主动关闭 = 放弃）；正常收尾走 closeReq
        {
            std::lock_guard<std::mutex> lk(g_prog.m);
            if (g_prog.closeReq) {
                EndDialog(h, 0);
                return TRUE;
            }
        }
            progress_cancel_now(h);
            return TRUE;
        default:
            return FALSE;
    }
    return FALSE;
}

void progress_thread_main() {
    init_comctl_once();
    {
        std::lock_guard<std::mutex> lk(g_prog.m);
        if (g_prog.closeReq) return;
    }
    TplBuilder b;
    b.header(L"nx 正在解压…", 300, 100);
    build_progress_tpl(b);
    finalize(b);
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
                            reinterpret_cast<const DLGTEMPLATE*>(b.data()), nullptr,
                            progress_proc, 0);
    {
        std::lock_guard<std::mutex> lk(g_prog.m);
        g_prog.hwnd = nullptr;
    }
}

} // namespace

std::optional<std::wstring> ask_password(const std::string& layerDescUtf8) {
    AskCtx ctx;
    ctx.caption = L"nx 需要密码";
    ctx.label = utf8_to_wide(layerDescUtf8);
    ctx.password = true;
    return run_input_dialog(ctx);
}

std::optional<std::wstring> ask_prefix(const std::wstring& defaultValue) {
    AskCtx ctx;
    ctx.caption = L"nx 解压到指定目录";
    ctx.label = L"输出前缀目录名（位于压缩文件所在目录下）：";
    ctx.prefill = defaultValue;
    return run_input_dialog(ctx);
}

void notify_done(bool ok, const std::string& detailUtf8, const wchar_t* titleOverride) {
    std::wstring msg = utf8_to_wide(detailUtf8);
    std::wstring title = titleOverride ? titleOverride
                                       : (ok ? L"nx 解压完成" : L"nx 解压失败");
    MessageBoxW(nullptr, msg.c_str(), title.c_str(),
                ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
}

// ---- 进度窗公开 API ----

void progress_show(const std::wstring& caption, Stats* stats, const InputMeter* meter) {
    progress_hide();   // 幂等：清掉上一次（若未 hide）
    // 全部状态先于线程启动写入（happens-before），GUI 线程只读
    g_prog.stats = stats;
    g_prog.meter = meter;
    g_prog.t0 = GetTickCount64();
    g_prog.cancelled.store(false);
    g_prog.active.store(true);
    {
        std::lock_guard<std::mutex> lk(g_prog.m);
        g_prog.closeReq = false;
        g_prog.hwnd = nullptr;
        g_prog.curLine.clear();
        g_prog.caption = caption;
        g_prog.thread = std::make_unique<std::jthread>(progress_thread_main);
    }
}

void progress_hide() {
    if (!g_prog.active.exchange(false))
        return;
    HWND h = nullptr;
    std::unique_ptr<std::jthread> t;
    {
        std::lock_guard<std::mutex> lk(g_prog.m);
        g_prog.closeReq = true;
        h = g_prog.hwnd;
        t = std::move(g_prog.thread);
    }
    if (h)
        PostMessageW(h, WM_CLOSE, 0, 0);   // 线程安全；WM_CLOSE 分支见 closeReq 直接收尾
    if (t)
        t->join();
}

bool progress_cancelled() {
    return g_prog.cancelled.load();
}

void progress_file(const std::string& relUtf8) {
    if (!g_prog.active.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lk(g_prog.m);
    g_prog.curLine = L"写出 " + utf8_to_wide(relUtf8);
}

void progress_stage(const std::string& stageUtf8) {
    if (!g_prog.active.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lk(g_prog.m);
    g_prog.curLine = utf8_to_wide(stageUtf8);
}

} // namespace nx::gui
