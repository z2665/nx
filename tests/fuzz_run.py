#!/usr/bin/env python3
"""nx fuzz 运行器：配置/构建 nxfuzz（libFuzzer+ASan）→ 组装种子语料 → 运行。

用法：
  python tests/fuzz_run.py                  # 默认跑 10 分钟
  python tests/fuzz_run.py --time 3600      # 跑 1 小时
  python tests/fuzz_run.py --jobs 4         # 4 进程并行（-fork=4）
  python tests/fuzz_run.py --rerun <file>   # 复现单个崩溃工件

种子语料：tests/cases（gen_corpus* 产物，gitignore）中 ≤1MiB 的文件全量拷入
tests/fuzz-corpus-seeds/；tests/fuzz-regression/（git 跟踪）的历史崩溃工件每次
回灌为 reg_* 种子（回归防线，评审 M-1）；累积语料 tests/fuzz-corpus/ 跨次运行
增长（libFuzzer 第一个目录为主语料，其余为种子）。崩溃工件（crash-*/timeout-*）
落在仓库根——修复后移入 fuzz-regression/ 并登记其 README。

构建目录 build-fuzz/（gitignore）与 build/ 完全独立：RelWithDebInfo + NX_FUZZ=ON，
vcpkg 依赖经二进制缓存复用，不重复编译。
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VSROOT = os.environ.get("NX_VSROOT", r"C:\Program Files\Microsoft Visual Studio\18\Community")
TOOLCHAIN = os.path.join(VSROOT, "VC", "vcpkg", "scripts", "buildsystems", "vcpkg.cmake")
BUILD = os.path.join(ROOT, "build-fuzz")
FUZZ_EXE = os.path.join(BUILD, "nxfuzz.exe")
CORPUS = os.path.join(HERE, "fuzz-corpus")
SEEDS = os.path.join(HERE, "fuzz-corpus-seeds")
CASES = os.path.join(HERE, "cases")
REGRESSION = os.path.join(HERE, "fuzz-regression")   # 持久回归种子（git 跟踪，评审 M-1）

MAX_SEED = 1 << 20  # 种子上限 1MiB（-max_len 同值）


def run_cmd(line: str) -> int:
    print(f"[fuzz] {line}")
    return subprocess.call(f'call "{VSROOT}\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul 2>&1 && {line}',
                           shell=True, cwd=ROOT)


def configure_and_build() -> int:
    rc = run_cmd(
        f'cmake -S . -B "{BUILD}" -G Ninja '
        f'-DCMAKE_BUILD_TYPE=RelWithDebInfo -DNX_FUZZ=ON '
        f'-DCMAKE_TOOLCHAIN_FILE="{TOOLCHAIN}" '
        f'-DVCPKG_TARGET_TRIPLET=x64-windows-static '
        f'-DVCPKG_INSTALL_OPTIONS="--overlay-ports={ROOT}/ports-overlay"'
    )
    if rc != 0:
        return rc
    return run_cmd(f'cmake --build "{BUILD}" --target nxfuzz')


def populate_seeds() -> int:
    if os.path.isdir(SEEDS):
        shutil.rmtree(SEEDS)
    os.makedirs(SEEDS)
    n = 0
    if os.path.isdir(CASES):
        for dirpath, _dirs, files in os.walk(CASES):
            for fn in files:
                full = os.path.join(dirpath, fn)
                try:
                    if os.path.getsize(full) > MAX_SEED:
                        continue
                except OSError:
                    continue
                shutil.copy2(full, os.path.join(SEEDS, f"seed_{n:05d}"))
                n += 1
    # 持久回归种子回灌（评审 M-1）：SEEDS 每次重建，历史崩溃工件只活在
    # fuzz-regression/（git 跟踪）——不回灌则回归种子被本函数静默抹掉
    if os.path.isdir(REGRESSION):
        for fn in sorted(os.listdir(REGRESSION)):
            full = os.path.join(REGRESSION, fn)
            if os.path.isfile(full) and not fn.startswith("README"):
                shutil.copy2(full, os.path.join(SEEDS, "reg_" + fn))
                n += 1
    return n


def main() -> int:
    ap = argparse.ArgumentParser(description="nx libFuzzer 运行器")
    ap.add_argument("--time", type=int, default=600, help="总时长（秒，默认 600）")
    ap.add_argument("--jobs", type=int, default=1, help="并行进程数（-fork=N，默认 1）")
    ap.add_argument("--rerun", metavar="FILE", help="复现单个工件（跑一次后退出）")
    args = ap.parse_args()

    if args.rerun:
        if not os.path.isfile(FUZZ_EXE):
            print("[fuzz] nxfuzz.exe 不存在，先构建", file=sys.stderr)
            return 1
        return subprocess.call([FUZZ_EXE, args.rerun])

    rc = configure_and_build()
    if rc != 0:
        print(f"[fuzz] 构建失败（exit {rc}）", file=sys.stderr)
        return rc

    n_seeds = populate_seeds()
    os.makedirs(CORPUS, exist_ok=True)
    print(f"[fuzz] 种子 {n_seeds} 个 · 语料 {CORPUS}")

    cmd = [FUZZ_EXE,
           "-max_len=1048576", "-timeout=25", "-rss_limit_mb=4096",
           "-close_fd_mask=1",   # 目标 stdout 静音（进程内已 quiet，防第三方库打印）
           "-max_total_time=" + str(args.time), "-print_final_stats=1"]
    if args.jobs > 1:
        cmd.append(f"-fork={args.jobs}")
    cmd += [CORPUS, SEEDS]
    print("[fuzz] " + " ".join(cmd))
    rc = subprocess.call(cmd, cwd=ROOT)
    if rc != 0:
        print("\n[fuzz] 非零退出（exit %d）——崩溃/超时工件见仓库根 crash-*/timeout-*；"
              "复现：python tests/fuzz_run.py --rerun <file>" % rc, file=sys.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main())
