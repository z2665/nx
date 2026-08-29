#!/usr/bin/env python3
"""M3 GUI 冒烟：前缀弹窗（输入/默认值/取消）+ 密码弹窗（多层异密码 + 取消）
+ 进度窗（出现/自动关闭 + 取消中止，待办 #1）。"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_auto import NxDialog

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
NX = os.path.join(ROOT, "build", "nx.exe")


def fresh(tmpname):
    tmp = tempfile.mkdtemp(prefix="nxgui_")
    return tmp


def rel_files(root):
    return sorted(os.path.relpath(os.path.join(d, f), root).replace("\\", "/")
                  for d, _, fs in os.walk(root) for f in fs)


def main():
    okAll = True

    # ---- 用例 1：前缀弹窗（默认=压缩文件名），输入 myprefix → 确定
    tmp = fresh("t1")
    shutil.copy(os.path.join(ROOT, "tests/cases/plain_zip/plain.zip"),
                os.path.join(tmp, "plain.zip"))
    p = subprocess.Popen([NX, "extract-into", os.path.join(tmp, "plain.zip")],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    d = NxDialog.wait_for(p.pid, "解压到指定目录")
    default = d.get_text()
    d.set_text("myprefix")
    d.ok()
    out, _ = p.communicate(timeout=20)
    found = rel_files(os.path.join(tmp, "myprefix"))
    ok = default == "plain.zip" and found == ["dir/a.bin", "readme.txt"] \
        and p.returncode == 0
    print(f"[1] 前缀弹窗 默认值={default!r} exit={p.returncode} found={found} → {'PASS' if ok else 'FAIL'}")
    okAll &= ok
    shutil.rmtree(tmp, ignore_errors=True)

    # ---- 用例 2：前缀弹窗 X → 取消退出（exit 2，无输出）
    tmp = fresh("t2")
    shutil.copy(os.path.join(ROOT, "tests/cases/plain_zip/plain.zip"),
                os.path.join(tmp, "plain.zip"))
    p = subprocess.Popen([NX, "extract-into", os.path.join(tmp, "plain.zip")],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    d = NxDialog.wait_for(p.pid, "解压到指定目录")
    d.close()
    p.communicate(timeout=10)
    ok = p.returncode == 2 and os.listdir(tmp) == ["plain.zip"]
    print(f"[2] 前缀取消 exit={p.returncode} 残留={os.listdir(tmp)} → {'PASS' if ok else 'FAIL'}")
    okAll &= ok
    shutil.rmtree(tmp, ignore_errors=True)

    # ---- 用例 3：双层异密码 GUI（rar_encrypted 单层即可：前缀 → 密码两窗串行）
    rar = os.path.join(ROOT, "tests/cases/rar_encrypted/vault.rar")
    if os.path.exists(rar):
        tmp = fresh("t3")
        shutil.copy(rar, os.path.join(tmp, "vault.rar"))
        p = subprocess.Popen([NX, "extract-into", os.path.join(tmp, "vault.rar")],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        d1 = NxDialog.wait_for(p.pid, "解压到指定目录")
        d1.set_text("out")
        d1.ok()
        d2 = NxDialog.wait_for(p.pid, "需要密码")
        title = d2.title()
        d2.set_text("RarPw@2024")
        d2.ok()
        out, _ = p.communicate(timeout=30)
        found = rel_files(os.path.join(tmp, "out"))
        ok = p.returncode == 0 and any("secret/a.txt" in f for f in found) and "密码" in title
        print(f"[3] 密码弹窗 标题含层身份={('密码' in title)} exit={p.returncode} "
              f"文件={len(found)} → {'PASS' if ok else 'FAIL'}")
        okAll &= ok
        shutil.rmtree(tmp, ignore_errors=True)

        # ---- 用例 4：密码弹窗取消 → 整体取消退出（exit 2）
        tmp = fresh("t4")
        shutil.copy(rar, os.path.join(tmp, "vault.rar"))
        p = subprocess.Popen([NX, "extract-into", os.path.join(tmp, "vault.rar")],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        d1 = NxDialog.wait_for(p.pid, "解压到指定目录")
        d1.ok()   # 用默认前缀 vault.rar
        d2 = NxDialog.wait_for(p.pid, "需要密码")
        d2.cancel()
        p.communicate(timeout=10)
        ok = p.returncode == 2
        print(f"[4] 密码取消 exit={p.returncode} → {'PASS' if ok else 'FAIL'}")
        okAll &= ok
        shutil.rmtree(tmp, ignore_errors=True)

    # ---- 用例 5：进度窗出现（含动画条/统计行）→ 正常完成后自动关闭 ----
    tmp = fresh("t5")
    zp = os.path.join(tmp, "big.zip")
    with zipfile.ZipFile(zp, "w", zipfile.ZIP_STORED) as z:
        z.writestr("zeros.bin", b"\0" * (512 << 20))   # 存储式 512M：确保跨过首个 200ms 定时刷新
    out = os.path.join(tmp, "out")
    p = subprocess.Popen([NX, "extract", "--gui", zp, "-O", out],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    d = NxDialog.wait_for(p.pid, "正在解压", timeout=8, interval=0.03)
    has_bar = d.has_progress_bar()
    stats_seen = False   # 统计行由 200ms 定时器刷新，轮询等待
    t0 = time.time()
    while time.time() - t0 < 3.0 and d.is_alive() and not stats_seen:
        stats_seen = any("已输出" in t for t in d.static_texts())
        time.sleep(0.05)
    while d.is_alive() and time.time() - t0 < 20:
        time.sleep(0.05)
    p.communicate(timeout=30)
    found = rel_files(out) if os.path.isdir(out) else []
    ok = has_bar and stats_seen and not d.is_alive() and p.returncode == 0 \
        and found == ["big.zip/zeros.bin"]
    print(f"[5] 进度窗 bar={has_bar} 统计行={stats_seen} exit={p.returncode} "
          f"found={found} → {'PASS' if ok else 'FAIL'}")
    okAll &= ok
    shutil.rmtree(tmp, ignore_errors=True)

    # ---- 用例 6：进度窗取消 → 中止（exit 2，半成品清理，先前完成的小文件保留）----
    tmp = fresh("t6")
    zp = os.path.join(tmp, "bigcancel.zip")
    with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("small.txt", "head")
        with z.open("huge.bin", "w") as f:   # 768M 零：解压写盘期间点取消
            chunk = b"\0" * (1 << 20)
            for _ in range(768):
                f.write(chunk)
    out = os.path.join(tmp, "out")
    p = subprocess.Popen([NX, "extract", "--gui", zp, "-O", out],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    d = NxDialog.wait_for(p.pid, "正在解压", timeout=8, interval=0.05)
    d.cancel()
    p.communicate(timeout=30)
    inner = os.path.join(out, "bigcancel.zip")   # 默认根目录层
    leftover = os.listdir(inner) if os.path.isdir(inner) else []
    ok = p.returncode == 2 and "small.txt" in leftover and "huge.bin" not in leftover \
        and not any(".nxpart-" in f for f in leftover)
    print(f"[6] 进度取消 exit={p.returncode} 残留={leftover} → {'PASS' if ok else 'FAIL'}")
    okAll &= ok
    shutil.rmtree(tmp, ignore_errors=True)

    print("GUI 冒烟:", "PASS" if okAll else "FAIL")
    return 0 if okAll else 1


if __name__ == "__main__":
    sys.exit(main())
