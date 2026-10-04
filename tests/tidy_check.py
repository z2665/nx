#!/usr/bin/env python3
"""clang-tidy 基线门：零警告基线，新增即 FAIL。

用法：python tests/tidy_check.py [--jobs N]     # 默认并行 min(8, cpu 数)
依赖：VS "C++ Clang tools for Windows" 组件的 clang-tidy（无需 vcvars——
      clang 自主发现 MSVC/Windows SDK 头）与 build/vcpkg_installed 的三方头。

flags 注记：经 clang-tidy 的 `--` 追加参数须用 clang 风格——/EHsc、/std:c++latest
等 cl 旗标在此路径不生效（曾表现为 throw 全报"exceptions disabled"）；以
-fexceptions/-std=c++23/-fms-compatibility 等表达，语义与 CMakeLists 一致。
"""
import argparse
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VSROOT = os.environ.get("NX_VSROOT", r"C:\Program Files\Microsoft Visual Studio\18\Community")
TIDY = os.path.join(VSROOT, "VC", "Tools", "Llvm", "x64", "bin", "clang-tidy.exe")
CONFIG = os.path.join(ROOT, ".clang-tidy")
INC = os.path.join(ROOT, "build", "vcpkg_installed", "x64-windows-static", "include")
SRC = os.path.join(ROOT, "src")

FLAGS = [
    "-std=c++23", "-fexceptions",
    "-fms-compatibility", "-fms-extensions", "-fdelayed-template-parsing",
    "-target", "x86_64-pc-windows-msvc",
    "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN", "-D_WIN32_WINNT=0x0A00",
    "-DCRT_SECURE_NO_WARNINGS",
    f"-I{SRC}", f"-I{INC}",
]

# fuzz_main.cpp 排除：它面向 libFuzzer 驱动构建（-DNX_FUZZ），其专属头在
# build-fuzz 工具链里，与本门 flags 不相交——fuzz 目标自有构建验证覆盖
TUS = sorted(f for f in os.listdir(SRC)
             if f.endswith(".cpp") and f != "fuzz_main.cpp")


def run_tu(tu: str) -> tuple[str, list[str]]:
    """返回 (tu, 违例行列表)。编译错误也计违例——与 audit fail-loud 同一纪律。"""
    p = subprocess.run([TIDY, os.path.join(SRC, tu), f"--config-file={CONFIG}", "--"] + FLAGS,
                       capture_output=True, text=True, encoding="utf-8", errors="replace",
                       timeout=600)
    out = p.stdout + p.stderr
    hits = [ln for ln in out.splitlines()
            if re.search(r"\b(warning|error):", ln) and "clang-tidy] " not in ln]
    return tu, hits


def main() -> int:
    if not os.path.exists(TIDY):
        print(f"[tidy] 缺 {TIDY}——VS 组件 'C++ Clang tools for Windows' 未安装")
        return 2
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int,
                    default=min(8, os.cpu_count() or 1))
    args = ap.parse_args()

    print(f"[tidy] {len(TUS)} TU · clang-tidy 基线（四检查）· jobs={args.jobs}")
    total = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for tu, hits in sorted(ex.map(run_tu, TUS)):
            if hits:
                total += len(hits)
                print(f"[tidy] ❌ {tu} × {len(hits)}:")
                for h in hits:
                    print(f"        {h}")
    if total:
        print(f"[tidy] ❌ {total} 处违例（基线为零警告——修复或 NOLINT 附理由）")
        return 1
    print(f"[tidy] ✅ 零警告：{len(TUS)} TU 全部通过基线")
    return 0


if __name__ == "__main__":
    sys.exit(main())
