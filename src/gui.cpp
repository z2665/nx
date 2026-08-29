#include "gui.hpp"
#include "util.hpp"
#include <windows.h>
#include <cstring>

namespace nx::gui {

namespace {

constexpr int IDC_LABEL = 1000;
constexpr int IDC_EDIT = 1001;

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

    size_t build(const AskCtx& ctx) {
        // DLGTEMPLATE: style, dwExtendedStyle, cdit, x, y, cx, cy, menu, class, title, [font]
        put32(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT);
        put32(0);                        // dwExtendedStyle
        cditPos_ = p_;
        put16(0);                        // cdit（后补）
        put16(0); put16(0); put16(262); put16(116);   // x y cx cy（DLU）
        put16(0x0000);                   // menu: 空串 = 无
        put16(0x0000);                   // class: 空串 = 默认对话框类（0x0081 是 Edit 原子，非法）
        putwstr(ctx.caption.c_str());
        put16(9);                       // DS_SETFONT：先字号
        putwstr(L"Segoe UI");           // 后字体名

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

void notify_done(bool ok, const std::string& detailUtf8) {
    std::wstring msg = utf8_to_wide(detailUtf8);
    MessageBoxW(nullptr, msg.c_str(), ok ? L"nx 解压完成" : L"nx 解压失败",
                ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
}

} // namespace nx::gui
