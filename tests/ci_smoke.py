"""CI 冒烟：stdlib 生成嵌套语料 → nx extract → 全树哈希比对 + 退出码契约抽查。

CI runner 上不复现本地完整门体系（TLA+/F*/clang-tidy/WinRAR 语料等缺工具链，
门就是门——那是本地开发纪律）；本脚本只保证发布产物的最低功能正确性。
覆盖：zip 嵌 zip 递归、tar.gz 过滤器链、.001/.002 分片拼接、退出码契约。

用法：python tests/ci_smoke.py <nx.exe 路径>
"""
import hashlib
import io
import os
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import zipfile


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build_corpus(root: pathlib.Path) -> dict:
    """生成语料，返回 期望相对路径 → 内容哈希（三组输入共用一个期望语义）。"""
    expect: dict[str, str] = {}

    # 载荷：可压缩 + 随机
    payload_txt = b"nx ci smoke\n" * 4000
    payload_bin = bytes(range(256)) * 512

    # 最内层 zip（输出镜像逻辑嵌套树：其内容落在 pack/inner.zip/ 之下）
    inner = root / "inner.zip"
    with zipfile.ZipFile(inner, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("readme.txt", payload_txt)
        zf.writestr("data/bin.dat", payload_bin)
    expect["pack/inner.zip/readme.txt"] = sha(payload_txt)
    expect["pack/inner.zip/data/bin.dat"] = sha(payload_bin)

    # tar.gz（过滤器链：gzip → tar；内容落在 pack/nested.tgz/ 之下）
    tgz_buf = io.BytesIO()
    with tarfile.open(fileobj=tgz_buf, mode="w:gz") as tf:
        for name, data in (("logs/app.log", payload_txt), ("blob.raw", payload_bin)):
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            tf.addfile(ti, io.BytesIO(data))
    expect["pack/nested.tgz/logs/app.log"] = sha(payload_txt)
    expect["pack/nested.tgz/blob.raw"] = sha(payload_bin)

    # 外层 zip：嵌 inner.zip + 嵌 nested.tgz + 普通文件
    outer = root / "outer.zip"
    with zipfile.ZipFile(outer, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.write(inner, "pack/inner.zip")
        zf.writestr("pack/nested.tgz", tgz_buf.getvalue())
        zf.writestr("top.txt", payload_txt)
    expect["top.txt"] = sha(payload_txt)
    inner.unlink()
    return expect


def run(nx: str, *args: str, cwd: str | None = None) -> subprocess.CompletedProcess:
    return subprocess.run([nx, *args], capture_output=True, text=True,
                          timeout=300, cwd=cwd, encoding="utf-8", errors="replace")


def tree_hashes(out_root: pathlib.Path) -> dict[str, str]:
    got = {}
    for p in sorted(out_root.rglob("*")):
        if p.is_file():
            rel = p.relative_to(out_root).as_posix()
            got[rel] = sha(p.read_bytes())
    return got


def strip_top(tree: dict[str, str]) -> dict[str, str]:
    """去掉根目录层（输入名目录），比对内部树。"""
    out = {}
    for rel, h in tree.items():
        parts = rel.split("/", 1)
        if len(parts) == 2:
            out[parts[1]] = h
    return out


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: ci_smoke.py <nx.exe>")
        return 2
    nx = os.path.abspath(sys.argv[1])
    if not os.path.isfile(nx):
        print(f"[ci] 被测 exe 不存在: {nx}")
        return 2

    # 退出码契约：无参数 = 用法错误 64
    r = run(nx)
    if r.returncode != 64:
        print(f"[ci] FAIL 无参数应退出 64，实际 {r.returncode}")
        return 1

    with tempfile.TemporaryDirectory(prefix="nx_ci_") as td:
        root = pathlib.Path(td)
        expect = build_corpus(root)

        # 用例 1：嵌套 zip（zip→zip→文件 + zip→tgz→文件）
        out1 = root / "out1"
        r = run(nx, "extract", str(root / "outer.zip"), "-O", str(out1), "--no-prompt")
        if r.returncode != 0:
            print(f"[ci] FAIL 嵌套 zip 退出码 {r.returncode}\n{r.stdout}\n{r.stderr}")
            return 1
        got = strip_top(tree_hashes(out1))
        if got != expect:
            print(f"[ci] FAIL 嵌套 zip 树不符\n期望: {expect}\n实际: {got}")
            return 1

        # 用例 2：分片 .001/.002 拼接（outer.zip 切两半 → data.zip.001/.002）
        raw = (root / "outer.zip").read_bytes()
        half = len(raw) // 2
        (root / "data.zip.001").write_bytes(raw[:half])
        (root / "data.zip.002").write_bytes(raw[half:])
        (root / "outer.zip").unlink()
        out2 = root / "out2"
        r = run(nx, "extract", str(root / "data.zip.001"), "-O", str(out2), "--no-prompt")
        if r.returncode != 0:
            print(f"[ci] FAIL 分片拼接 退出码 {r.returncode}\n{r.stdout}\n{r.stderr}")
            return 1
        got = strip_top(tree_hashes(out2))
        if got != expect:
            print(f"[ci] FAIL 分片拼接 树不符\n期望: {expect}\n实际: {got}")
            return 1

        # 用例 3：序号断档 = 退出码 4（注：孤卷 .001 无法与"单卷完整分片"区分——
        # 编号型无终卷标记，截断损坏走 exit 1 是设计内行为；断档才报缺分片）
        raw = (root / "data.zip.001").read_bytes()
        (root / "data.zip.002").unlink()
        (root / "data.zip.003").write_bytes(raw[:64])
        r = run(nx, "extract", str(root / "data.zip.001"), "-O", str(root / "out3"),
                "--no-prompt")
        if r.returncode != 4:
            print(f"[ci] FAIL 序号断档应退出 4，实际 {r.returncode}\n{r.stdout}\n{r.stderr}")
            return 1

    print("[ci] 冒烟通过：嵌套 zip / tar.gz 过滤器链 / 分片拼接 / 退出码契约")
    return 0


if __name__ == "__main__":
    sys.exit(main())
