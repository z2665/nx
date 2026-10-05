#!/usr/bin/env python3
"""GUI 冒烟：前缀弹窗（输入/默认值/取消）+ 密码弹窗（多层异密码 + 取消）
+ 进度窗（出现/自动关闭 + 取消中止）。

时序稳定设计（共享 CI runner 上曾偶发砍断）：
- 每用例独立 try/except——异常（窗口超时/进程超时）判该用例 FAIL 并继续后续，
  不再砍死整个脚本（失败位置曾随负载漂移即此症状）；
- 等窗超时 20s、大件进程超时 90~180s、进度采样自适应——全部有界且宽裕；
- 进程收尾统一经 _fin()：TimeoutExpired 先 kill 再收尸，不留孤儿 nx 进程；
- 用例 6 点取消前先等首文件落盘（窗口出现早于首个条目写出时，立即取消
  会导致"small.txt 保留"断言偶发失败）。
"""
import io
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


def fresh(_tmpname):
    t = tempfile.mkdtemp(prefix="nxgui_")
    _TMPS.append(t)
    return t


_TMPS = []


def rel_files(root):
    return sorted(os.path.relpath(os.path.join(d, f), root).replace("\\", "/")
                  for d, _, fs in os.walk(root) for f in fs)


def _fin(p, timeout):
    """有界收尾：超时先 kill 再收（不留孤儿进程），返回 (stdout, returncode|None)。"""
    try:
        out, _ = p.communicate(timeout=timeout)
        return out, p.returncode
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()
        return (out or "") + "\n[nxgui] 进程超时被杀", None


def _wait_file(path, timeout=10.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(path):
            return True
        time.sleep(0.05)
    return False


def case_1():
    tmp = fresh("t1")
    shutil.copy(os.path.join(ROOT, "tests/cases/plain_zip/plain.zip"),
                os.path.join(tmp, "plain.zip"))
    p = subprocess.Popen([NX, "extract-into", os.path.join(tmp, "plain.zip")],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    d = NxDialog.wait_for(p.pid, "解压到指定目录")
    default = d.get_text()
    d.ok()
    _, rc = _fin(p, 90)
    found = rel_files(os.path.join(tmp, "plain")) if os.path.isdir(os.path.join(tmp, "plain")) else []
    return default == "plain" and found == ["dir/a.bin", "readme.txt"] and rc == 0, \
        f"[1] 前缀弹窗(默认) 默认值={default!r} exit={rc} found={found}"


def case_2():
    tmp = fresh("t2")
    shutil.copy(os.path.join(ROOT, "tests/cases/plain_zip/plain.zip"),
                os.path.join(tmp, "plain.zip"))
    p = subprocess.Popen([NX, "extract-into", os.path.join(tmp, "plain.zip")],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    d = NxDialog.wait_for(p.pid, "解压到指定目录")
    d.close()
    _, rc = _fin(p, 30)
    return rc == 2 and os.listdir(tmp) == ["plain.zip"], \
        f"[2] 前缀取消 exit={rc} 残留={os.listdir(tmp)}"


def case_3():
    rar = os.path.join(ROOT, "tests/cases/rar_encrypted/vault.rar")
    if not os.path.exists(rar):
        return None, "[3] 密码弹窗：跳过（缺 rar_encrypted 语料）"
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
    _, rc = _fin(p, 120)
    found = rel_files(os.path.join(tmp, "out"))
    return rc == 0 and any("secret/a.txt" in f for f in found) and "密码" in title, \
        f"[3] 密码弹窗 标题含层身份={('密码' in title)} exit={rc} 文件={len(found)}"


def case_4():
    rar = os.path.join(ROOT, "tests/cases/rar_encrypted/vault.rar")
    if not os.path.exists(rar):
        return None, "[4] 密码取消：跳过（缺 rar_encrypted 语料）"
    tmp = fresh("t4")
    shutil.copy(rar, os.path.join(tmp, "vault.rar"))
    p = subprocess.Popen([NX, "extract-into", os.path.join(tmp, "vault.rar")],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    d1 = NxDialog.wait_for(p.pid, "解压到指定目录")
    d1.ok()   # 用默认前缀 vault.rar
    d2 = NxDialog.wait_for(p.pid, "需要密码")
    d2.cancel()
    _, rc = _fin(p, 30)
    return rc == 2, f"[4] 密码取消 exit={rc}"


def case_5():
    tmp = fresh("t5")
    zp = os.path.join(tmp, "big.zip")
    with zipfile.ZipFile(zp, "w", zipfile.ZIP_STORED) as z:
        with z.open("zeros.bin", "w") as f:   # 存储式 1.5G：解压提速后仍需跨过多个定时刷新周期
            chunk = b"\0" * (1 << 20)
            for _ in range(1536):
                f.write(chunk)
    out = os.path.join(tmp, "out")
    p = subprocess.Popen([NX, "extract", "--gui", zp, "-O", out],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    d = NxDialog.wait_for(p.pid, "正在解压", timeout=20, interval=0.03)
    has_bar = d.has_progress_bar()
    stats_seen = False   # 统计行由 200ms 定时器刷新，轮询等待
    positions = []       # PBM_GETPOS 采样（FileSeekView 计量 → 随根消耗爬升）
    # 采样自适应：进度爬到 ≥20 即止，慢盘最多等 15s（原固定 3s 在共享 runner
    # 上偶发采不到爬升）
    t0 = time.time()
    while time.time() - t0 < 15.0 and d.is_alive():
        if not stats_seen:
            stats_seen = any("已输出" in t for t in d.static_texts())
        if has_bar:
            pos = d.progress_pos()
            positions.append(pos)
            if stats_seen and len(positions) > 1 and max(positions) >= 20 \
                    and max(positions) > min(positions):
                break
        time.sleep(0.05)
    while d.is_alive() and time.time() - t0 < 90:
        time.sleep(0.1)
    _, rc = _fin(p, 150)
    found = rel_files(out) if os.path.isdir(out) else []
    pct_ok = positions and max(positions) >= 20 and max(positions) > min(positions)
    ok = has_bar and stats_seen and not d.is_alive() and rc == 0 \
        and found == ["big.zip/zeros.bin"] and bool(pct_ok)
    return ok, (f"[5] 进度窗 bar={has_bar} 统计行={stats_seen} 百分比={pct_ok}（pos "
                f"{min(positions) if positions else '-'}→{max(positions) if positions else '-'}）"
                f" exit={rc} found={found}")


def case_6():
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
    d = NxDialog.wait_for(p.pid, "正在解压", timeout=20, interval=0.05)
    # 等首文件落盘再取消：进度窗可能出现于任何条目写出之前，立即取消会让
    # "small.txt 保留"断言在慢机器上偶发失败
    inner = os.path.join(out, "bigcancel.zip")
    _wait_file(os.path.join(inner, "small.txt"), timeout=15)
    d.cancel()
    _, rc = _fin(p, 150)
    leftover = os.listdir(inner) if os.path.isdir(inner) else []
    ok = rc == 2 and "small.txt" in leftover and "huge.bin" not in leftover \
        and not any(".nxpart-" in f for f in leftover)
    return ok, f"[6] 进度取消 exit={rc} 残留={leftover}"


def case_7():
    sz7 = r"C:\Program Files\7-Zip\7z.exe"
    if not (os.path.exists(sz7) and os.path.exists(r"C:\Program Files\7-Zip\7z.dll")):
        return None, "[7] 7z 直读：跳过（未安装 7-Zip）"
    tmp = fresh("t7")
    src = os.path.join(tmp, "zeros.bin")
    with open(src, "wb") as f:
        f.truncate(384 << 20)   # 稀疏 384M 零
    zp = os.path.join(tmp, "big.7z")
    subprocess.run([sz7, "a", "-mx=1", zp, src], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    out = os.path.join(tmp, "out")
    p = subprocess.Popen([NX, "extract", "--gui", zp, "-O", out],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    d = NxDialog.wait_for(p.pid, "正在解压", timeout=20, interval=0.05)
    positions = []
    t0 = time.time()
    while time.time() - t0 < 15.0 and d.is_alive():
        positions.append(d.progress_pos())
        if len(positions) > 1 and max(positions) >= 20 and max(positions) > min(positions):
            break
        time.sleep(0.05)
    while d.is_alive() and time.time() - t0 < 90:
        time.sleep(0.1)
    _, rc = _fin(p, 180)
    found = rel_files(out) if os.path.isdir(out) else []
    pct_ok = positions and max(positions) >= 20 and max(positions) > min(positions)
    ok = rc == 0 and any("zeros.bin" in f for f in found) and bool(pct_ok)
    return ok, (f"[7] 7z 直读百分比={pct_ok}（pos "
                f"{min(positions) if positions else '-'}→{max(positions) if positions else '-'}）"
                f" exit={rc}")


def case_8():
    tmp = fresh("t8")
    mp4 = os.path.join(tmp, "clip.mp4")
    zbuf = io.BytesIO()
    with zipfile.ZipFile(zbuf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("flag.txt", "hidden\n" * 100)
    with open(mp4, "wb") as f:
        f.write((8 + 16).to_bytes(4, "big") + b"ftyp" + b"\x00\x00\x02\x00isomiso2mp41")
        f.write((8 + 1024).to_bytes(4, "big") + b"mdat" + b"\x00" * 1024)
        f.write((8).to_bytes(4, "big") + b"free")
        f.write(zbuf.getvalue())
    p = subprocess.Popen([NX, "extract-stego", mp4],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    d1 = NxDialog.wait_for(p.pid, "解压到指定目录")
    default = d1.get_text()
    d1.ok()
    _, rc = _fin(p, 60)
    found = rel_files(os.path.join(tmp, "clip_stego")) \
        if os.path.isdir(os.path.join(tmp, "clip_stego")) else []
    ok = default == "clip_stego" and rc == 0 and found == ["flag.txt"]
    return ok, f"[8] 隐写动词 默认前缀={default!r} exit={rc} found={found}"


def case_9():
    tmp = fresh("t9")
    shutil.copy(os.path.join(ROOT, "tests/cases/plain_zip/plain.zip"),
                os.path.join(tmp, "noext"))
    p = subprocess.Popen([NX, "extract-into", os.path.join(tmp, "noext")],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    d1 = NxDialog.wait_for(p.pid, "解压到指定目录")
    default = d1.get_text()
    d1.ok()   # 接受默认前缀 noext == 输入文件名 → 输出目录撞输入文件
    d2 = NxDialog.wait_for(p.pid, "创建输出目录失败")
    texts = " ".join(d2.static_texts())
    d2.close()
    _, rc = _fin(p, 30)
    ok = default == "noext" and rc == 1 and "同名" in texts \
        and not os.path.isdir(os.path.join(tmp, "noext"))
    return ok, f"[9] 撞名弹窗 默认前缀={default!r} exit={rc} 提示含同名指引={('同名' in texts)}"


CASES = [case_1, case_2, case_3, case_4, case_5, case_6, case_7, case_8, case_9]


def main():
    ok_all = True
    for fn in CASES:
        try:
            ok, msg = fn()
        except Exception as e:   # 单用例异常不再砍死整个脚本（曾致输出截断）
            ok, msg = False, f"[?] {fn.__name__} 异常: {type(e).__name__}: {e}"
        if ok is None:
            print(msg + " → SKIP")
        else:
            print(msg + f" → {'PASS' if ok else 'FAIL'}")
            ok_all &= ok
        for t in _TMPS:   # 用例级清理（异常路径同样回收）
            shutil.rmtree(t, ignore_errors=True)
        _TMPS.clear()
    print("GUI 冒烟:", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
