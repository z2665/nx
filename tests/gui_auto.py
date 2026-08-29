#!/usr/bin/env python3
"""GUI 自动化辅助：按 PID 找 nx 弹窗，直接向控件发消息（不依赖键盘焦点）。

用法（作为库）：
    from gui_auto import NxDialog
    d = NxDialog.wait_for(pid, title_contains="解压到指定目录", timeout=5)
    d.set_text("myprefix")
    d.ok()          # 或 d.cancel() 模拟取消 / X
"""
import ctypes
import ctypes.wintypes as wt
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)

WM_SETTEXT = 0x000C
WM_COMMAND = 0x0111
BM_CLICK = 0x00F5
WM_CLOSE = 0x0010
IDOK = 1
IDCANCEL = 2

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def _win_text(h):
    n = user32.GetWindowTextLengthW(h)
    buf = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(h, buf, n + 1)
    return buf.value


def _class_name(h):
    buf = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(h, buf, 64)
    return buf.value


def _list_windows():
    out = []

    @WNDENUMPROC
    def cb(h, _l):
        out.append(h)
        return True

    user32.EnumWindows(cb, 0)
    return out


class NxDialog:
    def __init__(self, hwnd):
        self.hwnd = hwnd
        self.pid = None
        r = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(r))
        self.pid = r.value

    @staticmethod
    def wait_for(pid, title_contains="", timeout=8.0, interval=0.15):
        t0 = time.time()
        while time.time() - t0 < timeout:
            for h in _list_windows():
                if not user32.IsWindowVisible(h):
                    continue
                r = wt.DWORD()
                user32.GetWindowThreadProcessId(h, ctypes.byref(r))
                if r.value != pid:
                    continue
                title = _win_text(h)
                if title_contains and title_contains not in title:
                    continue
                return NxDialog(h)
            time.sleep(interval)
        raise TimeoutError(f"未找到窗口（pid={pid}, 含 '{title_contains}'）")

    def title(self):
        return _win_text(self.hwnd)

    def _children(self):
        out = []
        child = wt.HWND(0)
        while True:
            child = user32.FindWindowExW(self.hwnd, child, None, None)
            if not child:
                break
            out.append(child)
        return out

    def find_edit(self):
        for h in self._children():
            if _class_name(h).lower() == "edit":
                return h
        raise RuntimeError("未找到编辑框")

    def find_button(self, text):
        for h in self._children():
            if _class_name(h).lower() == "button" and _win_text(h) == text:
                return h
        raise RuntimeError(f"未找到按钮 {text}")

    def set_text(self, value):
        user32.SendMessageW(self.find_edit(), WM_SETTEXT, 0, value)

    def get_text(self):
        h = self.find_edit()
        n = user32.SendMessageW(h, 0x000E, 0, 0)  # WM_GETTEXTLENGTH
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.SendMessageW(h, 0x000D, n + 1, buf)  # WM_GETTEXT
        return buf.value

    def ok(self):
        user32.SendMessageW(self.find_button("确定"), BM_CLICK, 0, 0)

    def cancel(self):
        user32.SendMessageW(self.find_button("取消"), BM_CLICK, 0, 0)

    def close(self):   # 模拟点 X
        user32.PostMessageW(self.hwnd, WM_CLOSE, 0, 0)
