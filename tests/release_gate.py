#!/usr/bin/env python3
"""发布门：**合成发布语料**的端到端哈希比对——发布 nx.exe 前必跑。

语料 = gen_release_corpus.py 按真实案例结构重建（隐私纪律：真实样本/路径/密码
不入仓）；确定性种子使 manifest 跨再生成稳定。--spool-ram 16MiB 强制溢出磁盘
路径，小体积覆盖 15GB 级案例曾触发的 spool 往返/残留行为面。

用法：
  python tests/release_gate.py --update    # 生成语料（缺则）+ 刷新 manifest
  python tests/release_gate.py             # 校验：解压全部样本比对 manifest
  python tests/release_gate.py --sample L  # 只跑单样本
依赖：7z CLI（语料生成）；build\nx.exe（NX_EXE 可覆盖）。
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.environ.get("NX_EXE", os.path.join(ROOT, "build", "nx.exe"))
CASES = os.path.join(HERE, "release-cases")
MANIFEST = os.path.join(HERE, "release-gate-manifest.json")
WORK = os.path.join(HERE, "work", "release_gate")

import gen_release_corpus as grc   # noqa: E402（需 HERE 先就位）

SAMPLES = {
    "L": {   # 案例 L 结构：伪装 MP4 → 隐写 zip → AES zip（32 条目深路径大件）
        "file": "release_L.mp4",
        "args": lambda p, out: ["extract", "--stego", "-p", grc.PW_L,
                                "--spool-ram", "16MiB", p, "-O", out],
    },
    "XJ": {  # 案例 XJ 结构：伪装 MP4 → 隐写 zip → 7z SFX solid+AES（+.save 递归）
        "file": "release_XJ.mp4",
        "args": lambda p, out: ["extract", "--stego", "-p", grc.PW_XJ,
                                "--spool-ram", "16MiB", p, "-O", out],
    },
    "X": {   # 案例 X 结构：无扩展名 7z 嵌加密 zip
        "file": "release_x",
        "args": lambda p, out: ["extract", "-p", grc.PW_X,
                                "--spool-ram", "16MiB", p, "-O", out],
    },
}


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ensure_corpus() -> bool:
    want = {c["file"] for c in SAMPLES.values()}
    have = set(os.listdir(CASES)) if os.path.isdir(CASES) else set()
    if want <= have:
        return True
    print("[gate] 语料缺失，重建（确定性种子）…")
    return subprocess.call([sys.executable, os.path.join(HERE, "gen_release_corpus.py")]) == 0


def run_sample(name: str, cfg: dict) -> dict:
    out = os.path.join(WORK, name)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    p = os.path.join(CASES, cfg["file"])
    cmd = [EXE] + cfg["args"](p, out)
    print(f"[gate] {name}: {cfg['file']} …", flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", cwd=ROOT, timeout=1800)
    files = []
    for dirpath, _dirs, names in os.walk(out):
        for fn in sorted(names):
            full = os.path.join(dirpath, fn)
            files.append({"path": os.path.relpath(full, out).replace("\\", "/"),
                          "size": os.path.getsize(full),
                          "sha256": sha256_of(full)})
    files.sort(key=lambda x: x["path"])
    return {"exit": r.returncode, "fileCount": len(files), "files": files,
            "stderrTail": (r.stderr or "")[-300:] if r.returncode else ""}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true", help="生成语料（缺则）+ 刷新 manifest")
    ap.add_argument("--sample", choices=SAMPLES, help="只跑指定样本")
    args = ap.parse_args()

    if not os.path.exists(EXE):
        print(f"[gate] 缺 {EXE}——先 build.cmd")
        return 2
    if not ensure_corpus():
        print("[gate] 语料生成失败")
        return 2
    names = [args.sample] if args.sample else list(SAMPLES)

    manifest = {}
    if os.path.exists(MANIFEST):
        with open(MANIFEST, encoding="utf-8") as f:
            manifest = json.load(f)

    fails = 0
    for name in names:
        result = run_sample(name, SAMPLES[name])
        if args.update:
            manifest[name] = result
            print(f"[gate] ✅ {name}: manifest 已记录（exit {result['exit']} · "
                  f"{result['fileCount']} 文件）")
            continue
        want = manifest.get(name)
        if want is None:
            print(f"[gate] ❌ {name}: manifest 无记录——先 --update 固化基线")
            fails += 1
        elif result["exit"] != want["exit"] or result["files"] != want["files"]:
            fails += 1
            print(f"[gate] ❌ {name}: 与 manifest 不符（exit {result['exit']} vs "
                  f"{want['exit']}，文件 {result['fileCount']} vs {want['fileCount']}）"
                  f"{result['stderrTail']}")
            for a, b in zip(want["files"], result["files"]):
                if a != b:
                    print(f"        首个差异: {a['path']}")
                    break
        else:
            print(f"[gate] ✅ {name}: exit {result['exit']} · {result['fileCount']} 文件哈希全符")

    if args.update:
        with open(MANIFEST, "w", encoding="utf-8") as f:
            json.dump(manifest, f, ensure_ascii=False, indent=1)
        print(f"[gate] manifest 写入 {os.path.relpath(MANIFEST, ROOT)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
