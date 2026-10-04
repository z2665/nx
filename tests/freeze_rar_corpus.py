#!/usr/bin/env python3
"""冻结 rar 族测试语料到 tests/cases-rar/（git LFS 入库）。

背景：rar 语料只能由 WinRAR 试用版 Rar.exe 生成（专有，gitignored 仅本地），
无该工具的环境（CI）历史上跳过全部 rar 用例。本脚本把**当前本地已生成**的
rar 用例（gen_corpus_m1.py 产物 + gen_corpus.py 的 stego_mp4_rar）整目录冻结，
含 expected.json 与 SHA256SUMS 完整性清单——无 WinRAR 的环境由 CI 把本目录
拷入 tests/cases 后照常运行（读取引擎 7z.dll/libarchive 与生成工具无关）。

用法（本地有 WinRAR，先跑 gen_corpus.py + gen_corpus_m1.py 再跑本脚本）：
  python tests/freeze_rar_corpus.py            # 冻结（增量：已存在则跳过）
  python tests/freeze_rar_corpus.py --force    # 覆盖重冻

注意：冻结的是生成时的随机载荷（os.urandom）——每次重冻字节不同属预期；
expected.json 与语料同批冻结，二者永远一致。语料再生成（本地）不影响本目录。
"""
import argparse
import hashlib
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(HERE, "cases")
FROZEN = os.path.join(HERE, "cases-rar")

# WinRAR 依赖的 7 个用例（与 run_tests 跳过清单一一对应；rar4 除外——
# WinRAR 7.x 已不支持 -ma4，无生成渠道，见 README 已知限制）
RAR_CASES = [
    "rar5_plain",
    "rar_solid",
    "rar_encrypted",
    "rar_multivol",
    "rar_entry_level_volumes",
    "triple_chain",
    "stego_mp4_rar",
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true", help="覆盖重冻")
    args = ap.parse_args()

    missing = [c for c in RAR_CASES if not os.path.isdir(os.path.join(CASES, c))]
    if missing:
        print(f"[freeze] 缺少语料 {missing}——先跑 gen_corpus.py + gen_corpus_m1.py（需 WinRAR/7z）")
        return 2

    os.makedirs(FROZEN, exist_ok=True)
    lines = []
    for case in RAR_CASES:
        dst = os.path.join(FROZEN, case)
        if os.path.isdir(dst) and not args.force:
            print(f"[freeze] 跳过已冻结 {case}（--force 覆盖）")
        else:
            shutil.rmtree(dst, ignore_errors=True)
            shutil.copytree(os.path.join(CASES, case), dst)
            print(f"[freeze] {case}")
        for root, _, files in os.walk(dst):
            for fn in sorted(files):
                p = os.path.join(root, fn)
                rel = os.path.relpath(p, FROZEN).replace("\\", "/")
                h = hashlib.sha256(open(p, "rb").read()).hexdigest()
                lines.append(f"{h}  {rel}")

    manifest = os.path.join(FROZEN, "SHA256SUMS")
    open(manifest, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print(f"[freeze] 完成：{FROZEN}（{len(lines)} 个文件入清单）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
