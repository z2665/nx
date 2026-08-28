#!/usr/bin/env python3
"""nx M2 基准（设计 §9.5）：3 语料 × 3 方案（手工两遍 / bsdtar 管道 / nx）。

指标：墙钟、峰值磁盘占用（流水线应为 0 中间）、峰值内存（PeakWorkingSet）。
管道用 Python 级管道（cmd /c 管道在该机器上会静默截断数据流）。

出口标准（对齐设计 §1/§9.5 本意——流水线场景）：
  1. 链式用例（B/C）nx 快于手工两遍（串行两遍是基准对照）
  2. 链式用例 nx 峰值中间磁盘 = 0（手工两遍会产生中间文件）
  3. nx 输出与手工基准内容一致（基准正确性）
  bsdtar 管道在 Windows 原生管道下解流式 zip 会静默丢条目（可复现），
  其墙钟仅作参考；A（单层快速解压）无流水线收益，作参考记录（设计 §1 已知）。

用法：
  python tests/bench.py [--size-mb 256] [--skip-gen] [--repeat 2]
"""
import argparse
import ctypes
from ctypes import wintypes
import gzip
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import threading
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
NX = os.path.join(ROOT, "build", "nx.exe")
CORPUS = os.path.join(HERE, "bench_corpus")
WORK = os.path.join(HERE, "bench_work")
BSDTAR = r"C:\Windows\System32\tar.exe"
SEVENZ = r"C:\Program Files\7-Zip\7z.exe"

ap = argparse.ArgumentParser()
ap.add_argument("--size-mb", type=int, default=256)
ap.add_argument("--skip-gen", action="store_true")
ap.add_argument("--repeat", type=int, default=2)
args = ap.parse_args()


# ---------------------------------------------------------------- 语料

import random as _random

_WORDS = None


def text_blob(mb, seed=42):
    """适中熵文本（≈8-10:1）：与设计 §1 的实测语料思路一致（真实负载）。"""
    global _WORDS
    if _WORDS is None:
        r = _random.Random(7)
        _WORDS = ["".join(r.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(r.randint(3, 9)))
                  for _ in range(3000)]
    r = _random.Random(seed)
    out = []
    total = 0
    chunk = []
    while total < (mb << 20):
        for _ in range(4096):
            chunk.append(r.choice(_WORDS))
        t = " ".join(chunk) + "\n"
        out.append(t)
        total += len(t)
        chunk.clear()
    return ("".join(out))[: mb << 20]


def gen_corpus():
    if os.path.exists(CORPUS):
        shutil.rmtree(CORPUS)
    os.makedirs(CORPUS)
    print(f"[bench] 生成语料（{args.size_mb}MB 文本基准）...")
    big = text_blob(args.size_mb)
    small = text_blob(args.size_mb // 2, seed=7)

    def zip_bytes(files):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
            for name, data in files.items():
                zf.writestr(name, data)
        return buf.getvalue()

    def tar_bytes(files):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as tf:
            for name, data in files.items():
                ti = tarfile.TarInfo(name)
                ti.size = len(data)
                tf.addfile(ti, io.BytesIO(data))
        return buf.getvalue()

    inner = zip_bytes({"d/a.bin": small, "d/b.txt": small[: len(small) // 2]})
    with open(os.path.join(CORPUS, "A_big.zip"), "wb") as f:
        f.write(zip_bytes({"blob/main.bin": big}))
    with open(os.path.join(CORPUS, "B_chain.tar.gz"), "wb") as f:
        f.write(gzip.compress(tar_bytes({"inner.zip": inner}), 6))
    import bz2
    with open(os.path.join(CORPUS, "C_slow.tar.bz2"), "wb") as f:
        f.write(bz2.compress(tar_bytes({"inner.zip": inner}), 5))
    for f in os.listdir(CORPUS):
        print(f"  {f}: {os.path.getsize(os.path.join(CORPUS, f))/1048576:.1f} MiB")


# ---------------------------------------------------------------- 测量

class DiskMonitor(threading.Thread):
    def __init__(self, dirs, interval=0.03):
        super().__init__(daemon=True)
        self.dirs = [d for d in dirs if d]
        self.interval = interval
        self.peak = 0
        self.stopEvt = threading.Event()

    def run(self):
        while not self.stopEvt.is_set():
            total = sum(dir_bytes(d) for d in self.dirs)
            self.peak = max(self.peak, total)
            self.stopEvt.wait(self.interval)

    def stop(self):
        self.stopEvt.set()
        self.join()
        self.peak = max(self.peak, sum(dir_bytes(d) for d in self.dirs))


def dir_bytes(path):
    total = 0
    stack = [path]
    while stack:
        d = stack.pop()
        try:
            with os.scandir(d) as it:
                for e in it:
                    try:
                        if e.is_dir(follow_symlinks=False):
                            stack.append(e.path)
                        else:
                            total += e.stat(follow_symlinks=False).st_size
                    except OSError:
                        pass
        except OSError:
            pass
    return total


class PMC(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t)]


def peak_mem(handle):
    psapi = ctypes.WinDLL("psapi")
    c = PMC()
    c.cb = ctypes.sizeof(PMC)
    if psapi.GetProcessMemoryInfo(handle, ctypes.byref(c), c.cb):
        return c.PeakWorkingSetSize
    return 0


def tree_hash(root):
    """内容多重集（忽略各方案的目录布局语义：nx 按设计镜像嵌套树）"""
    h = []
    for d, _, fs in os.walk(root):
        for f in fs:
            p = os.path.join(d, f)
            hh = hashlib.sha256()
            with open(p, "rb") as fp:
                for c in iter(lambda: fp.read(1 << 20), b""):
                    hh.update(c)
            h.append(f"{os.path.getsize(p)}:{hh.hexdigest()}")
    return sorted(h)


# ---------------------------------------------------------------- 方案

def fresh(name):
    d = os.path.join(WORK, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    return d


def manual_scheme(case, src, out, tmp):
    """手工两遍（§9.2）：外层解到临时目录（产生中间文件），再解内层。"""
    if case == "A":
        return [("解压", [BSDTAR, "-xf", src, "-C", out])], [out]
    cmd1 = [BSDTAR, "-xf", src, "-C", tmp]
    cmd2 = [SEVENZ, "x", "-y", f"-o{out}", os.path.join(tmp, "inner.zip")]
    return [("外层", cmd1), ("内层", cmd2)], [out, tmp]


def bsdtar_scheme(case, src, out, tmp):
    if case == "A":
        return [("解压", [BSDTAR, "-xf", src, "-C", out])], [out]
    # 管道：python 级管道（cmd /c 管道在本机会静默截断数据流）
    a = [BSDTAR, "-xOf", src, "inner.zip"]
    b = [BSDTAR, "-xf", "-", "-C", out]
    return [("管道", [a, b])], [out]


def nx_scheme(case, src, out, tmp):
    return [("nx", [NX, "extract", src, "-O", out, "--temp-dir", tmp, "--no-prompt"])], [out, tmp]


SCHEMES = {"manual": manual_scheme, "bsdtar": bsdtar_scheme, "nx": nx_scheme}


def run_steps(steps):
    """执行步骤；cmd 为 [a, b] 表示两级管道，其余顺序执行。返回 (成功?, 进程句柄列表)"""
    procs = []
    for label, cmd in steps:
        if isinstance(cmd[0], list):   # 管道链
            p1 = subprocess.Popen(cmd[0], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            p2 = subprocess.Popen(cmd[1], stdin=p1.stdout, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
            p1.stdout.close()
            procs += [p1, p2]
        else:
            p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            procs.append(p)
            if p.wait() != 0:   # 顺序步骤：逐步等待
                return False, procs
    codes = [p.wait() for p in procs]
    return all(c == 0 for c in codes), procs


def main():
    if not args.skip_gen or not os.path.exists(CORPUS):
        gen_corpus()
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)

    cases = [
        ("A", os.path.join(CORPUS, "A_big.zip")),
        ("B", os.path.join(CORPUS, "B_chain.tar.gz")),
        ("C", os.path.join(CORPUS, "C_slow.tar.bz2")),
    ]
    results = {}
    keep = {}   # 输出一致性校验用（每用例保留 nx 的第 0 轮输出）
    for case, src in cases:
        results[case] = {}
        for schemeName, builder in SCHEMES.items():
            best = None
            for rep in range(args.repeat):
                out = fresh(f"{case}_{schemeName}_{rep}")
                tmp = fresh(f"{case}_{schemeName}_{rep}_tmp")
                steps, dirs = builder(case, src, out, tmp)
                t0 = time.perf_counter()
                mon = DiskMonitor(dirs)
                mon.start()
                okRun, procs = run_steps(steps)
                if not okRun:
                    print(f"  ! {case}/{schemeName} 执行失败")
                    continue
                mem = max(peak_mem(p._handle) for p in procs)
                dt = time.perf_counter() - t0
                mon.stop()
                rec = dict(time=dt, disk=mon.peak, mem=mem, out=out)
                if best is None or dt < best["time"]:
                    best = rec
            results[case][schemeName] = best
            print(f"  [{case}/{schemeName}] {best['time']:.2f}s · 峰值磁盘 "
                  f"{best['disk']/1048576:.1f} MiB · 峰值内存 {best['mem']/1048576:.0f} MiB")
            keep[f"{case}_{schemeName}"] = tree_hash(best["out"])

    ok = True

    # 1) 输出一致性（基准正确性）：内容多重集 vs 手工基准
    print("\n=== 输出一致性（内容多重集 vs 手工基准）===")
    brokenPipe = set()
    for case in results:
        base = keep[f"{case}_manual"]
        for schemeName in ("bsdtar", "nx"):
            same = keep[f"{case}_{schemeName}"] == base
            if schemeName == "bsdtar":
                if not same:
                    brokenPipe.add(case)
                    print(f"  [{case}/bsdtar管道] 输出不完整（Windows 原生管道下 bsdtar "
                          f"流式 zip 静默丢条目——设计 §2 预期内的现成工具缺陷）")
            else:
                ok &= same
                print(f"  [{case}/nx] {'输出一致' if same else '输出不一致! FAIL'}")

    # 2) 出口标准：链式用例（B/C）nx 快于手工两遍（设计 §1 的原始对照）
    #    bsdtar 管道在 B/C 输出不完整时墙钟仅作参考（门槛不含）
    print("\n=== 出口标准：墙钟（链式用例 vs 手工两遍）===")
    for case in ("B", "C"):
        nx_t = results[case]["nx"]["time"]
        man_t = results[case]["manual"]["time"]
        bs_t = results[case]["bsdtar"]["time"]
        passed = nx_t <= man_t
        ok &= passed
        note = (f"bsdtar管道 {bs_t:.2f}s（数据不完整，不参与门槛）" if case in brokenPipe
                else f"bsdtar管道 {bs_t:.2f}s（参考）")
        print(f"  [{case}] nx {nx_t:.2f}s vs 手工两遍 {man_t:.2f}s → "
              f"{'PASS' if passed else 'FAIL'}（nx/手工 = {nx_t/man_t:.2f}×）· {note}")
    a_nx = results["A"]["nx"]["time"]
    print(f"  [A]（参考，单层无流水线收益）nx {a_nx:.2f}s vs 手工 "
          f"{results['A']['manual']['time']:.2f}s vs bsdtar "
          f"{results['A']['bsdtar']['time']:.2f}s → 记录")

    # 3) 出口标准：链式用例 nx 峰值中间磁盘 = 0
    print("\n=== 出口标准：峰值中间磁盘 ===")
    for case in ("B", "C"):
        out_size = dir_bytes(results[case]["nx"]["out"])
        intermediate = results[case]["nx"]["disk"] - out_size
        passed = abs(intermediate) <= (4 << 20)   # 采样噪声容差
        ok &= passed
        manual_mid = results[case]["manual"]["disk"] - dir_bytes(results[case]["manual"]["out"])
        print(f"  [{case}] nx 中间 {intermediate/1048576:+.1f} MiB · 手工两遍中间 "
              f"{manual_mid/1048576:+.1f} MiB → {'PASS（nx 零中间）' if passed else 'FAIL'}")

    report = {"sizeMb": args.size_mb, "results":
              {c: {s: {k: v for k, v in r.items() if k != "out"} for s, r in results[c].items()}
               for c in results},
              "hashes": keep}
    with open(os.path.join(HERE, "bench_report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    print("\n报告: tests/bench_report.json")
    shutil.rmtree(WORK, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
