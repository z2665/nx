#!/usr/bin/env python3
"""M3 GUI 冒烟：前缀弹窗（输入/默认值/取消）+ 密码弹窗（多层异密码 + 取消）。"""
import os
import shutil
import subprocess
import sys
import tempfile

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

    print("GUI 冒烟:", "PASS" if okAll else "FAIL")
    return 0 if okAll else 1


if __name__ == "__main__":
    sys.exit(main())
