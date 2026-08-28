#!/usr/bin/env python3
"""nx M0 测试语料生成器（设计 §9.1）。

生成 tests/cases/<name>/ 输入文件 + expected.json（输出相对路径 → sha256）。
输出路径契约（与 walker 实现一致）：
  outDir/<根名(去分片后缀)>/<嵌套层名>/…/<文件>
  裸过滤器根（plain.txt.gz）例外：outDir/<去后缀文件名>

依赖：Python 3.8+ 标准库；7z CLI（加密 zip / 7z 用例）。
"""
import bz2
import gzip
import hashlib
import io
import json
import lzma
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile

SEVEN_ZIP = r"C:\Program Files\7-Zip\7z.exe"
CASES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cases")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def make_files(spec: dict) -> dict:
    return {name: (c.encode("utf-8") if isinstance(c, str) else c)
            for name, c in spec.items()}


def tar_bytes(files: dict) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        for name, data in files.items():
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            tf.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def zip_bytes(files: dict) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, data in files.items():
            zf.writestr(name, data)
    return buf.getvalue()


def tree_hash(files: dict) -> dict:
    return {k: sha256(v) for k, v in files.items()}


def write_case(name: str, builder, ground_truth: dict):
    case_dir = os.path.join(CASES_DIR, name)
    if os.path.exists(case_dir):
        shutil.rmtree(case_dir)
    os.makedirs(case_dir)
    builder(case_dir)
    with open(os.path.join(case_dir, "expected.json"), "w", encoding="utf-8") as f:
        json.dump({"files": ground_truth}, f, indent=1, ensure_ascii=False)
    print(f"[gen] {name}: {len(ground_truth)} 个期望文件")


def sevenz_available() -> bool:
    return os.path.exists(SEVEN_ZIP)


def build_staging(d, files, sub="_s"):
    staging = os.path.join(d, sub)
    if os.path.exists(staging):
        shutil.rmtree(staging)
    for name, data in files.items():
        p = os.path.join(staging, name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
    return staging


# ---------------------------------------------------------------- 用例

def case_plain_zip():
    files = make_files({"readme.txt": "hello nx\n" * 100,
                        "dir/a.bin": os.urandom(65536)})
    def build(d):
        with open(os.path.join(d, "plain.zip"), "wb") as f:
            f.write(zip_bytes(files))
    write_case("plain_zip", build, {f"plain.zip/{k}": v for k, v in tree_hash(files).items()})


def case_three_layer():
    """3 层嵌套：data.tar.gz → inner.zip → 文件（M0 出口标准：零中间文件）"""
    inner = make_files({"a.txt": "layer3-a\n" * 50,
                        "sub/b.dat": os.urandom(200000)})
    tar_gz = gzip.compress(tar_bytes({"inner.zip": zip_bytes(inner), "loose.txt": b"loose\n"}))
    def build(d):
        with open(os.path.join(d, "data.tar.gz"), "wb") as f:
            f.write(tar_gz)
    expected = {f"data.tar.gz/inner.zip/{k}": v for k, v in tree_hash(inner).items()}
    expected["data.tar.gz/loose.txt"] = sha256(b"loose\n")
    write_case("three_layer", build, expected)


def case_split_zip():
    """分片：data.zip.001/.002/.003 → zip → inner.tar.gz → 文件"""
    inner = make_files({"f1.bin": os.urandom(150000), "f2.txt": "split me\n" * 1000})
    z = zip_bytes({"inner.tar.gz": gzip.compress(tar_bytes(inner))})
    part = 64 * 1024
    parts = [z[i:i + part] for i in range(0, len(z), part)]
    def build(d):
        for i, p in enumerate(parts):
            with open(os.path.join(d, f"data.zip.{i+1:03d}"), "wb") as f:
                f.write(p)
    expected = {f"data.zip/inner.tar.gz/{k}": v for k, v in tree_hash(inner).items()}
    write_case("split_zip", build, expected)


def case_split_entry_level():
    """条目级分片：outer.tar.gz 内含 zip 分片散落条目"""
    inner = make_files({"deep.txt": "entry-level split\n" * 500})
    z = zip_bytes(inner)
    part = 32 * 1024
    parts = [z[i:i + part] for i in range(0, len(z), part)]
    members = {f"data.zip.{i+1:03d}": p for i, p in enumerate(parts)}
    def build(d):
        with open(os.path.join(d, "outer.tar.gz"), "wb") as f:
            f.write(gzip.compress(tar_bytes(members)))
    expected = {f"outer.tar.gz/data.zip/{k}": v for k, v in tree_hash(inner).items()}
    write_case("split_entry_level", build, expected)


def case_multimember_gz():
    """多成员 gzip 串联（§3.1 注 / §10）"""
    a = b"first member\n" * 1000
    b = b"second member\n" * 2000
    inner = gzip.compress(a) + gzip.compress(b)
    def build(d):
        with open(os.path.join(d, "data.tar.gz"), "wb") as f:
            f.write(gzip.compress(tar_bytes({"concat.gz": inner, "note.txt": b"n\n"})))
    write_case("multimember_gz", build,
               {"data.tar.gz/concat": sha256(a + b),
                "data.tar.gz/note.txt": sha256(b"n\n")})


def case_two_passwords():
    """两层异密码：outer.zip(OuterPw@2024) → inner.zip(InnerPw@2024) → 文件"""
    if not sevenz_available():
        print("[gen] 跳过 two_passwords（无 7z CLI）")
        return
    inner = make_files({"secret1.txt": "pwpwpw\n" * 300, "s2/secret2.bin": os.urandom(100000)})
    def build(d):
        s1 = build_staging(d, inner, "_s1")
        subprocess.run([SEVEN_ZIP, "a", "-tzip", "-mem=AES256", "-pInnerPw@2024",
                        os.path.abspath(os.path.join(d, "_inner.zip")), "."],
                       check=True, capture_output=True, cwd=s1)
        s2 = build_staging(d, {"inner.zip": open(os.path.join(d, "_inner.zip"), "rb").read()}, "_s2")
        os.remove(os.path.join(d, "_inner.zip"))
        subprocess.run([SEVEN_ZIP, "a", "-tzip", "-mem=AES256", "-pOuterPw@2024",
                        os.path.abspath(os.path.join(d, "outer.zip")), "."],
                       check=True, capture_output=True, cwd=s2)
        shutil.rmtree(s1)
        shutil.rmtree(s2)
    expected = {f"outer.zip/inner.zip/{k}": v for k, v in tree_hash(inner).items()}
    write_case("two_passwords", build, expected)


def case_7z_nested():
    """R 类：outer.tar.gz → inner.7z（spool 路径）"""
    if not sevenz_available():
        print("[gen] 跳过 7z_nested（无 7z CLI）")
        return
    inner = make_files({"seven.txt": "7z inside\n" * 800, "d/x.bin": os.urandom(300000)})
    def build(d):
        s = build_staging(d, inner)
        subprocess.run([SEVEN_ZIP, "a", "-t7z", os.path.abspath(os.path.join(d, "inner.7z")), "."],
                       check=True, capture_output=True, cwd=s)
        shutil.rmtree(s)
        with open(os.path.join(d, "inner.7z"), "rb") as f:
            z7 = f.read()
        os.remove(os.path.join(d, "inner.7z"))
        with open(os.path.join(d, "outer.tar.gz"), "wb") as f:
            f.write(gzip.compress(tar_bytes({"inner.7z": z7})))
    expected = {f"outer.tar.gz/inner.7z/{k}": v for k, v in tree_hash(inner).items()}
    write_case("7z_nested", build, expected)


def case_zip_slip():
    """对抗：zip-slip 路径（D6 消毒：.. 段改写 __）"""
    def build(d):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("../../evil.txt", b"evil\n")
            zf.writestr("ok.txt", b"ok\n")
            zf.writestr("dir/../dotdot.txt", b"dd\n")
        with open(os.path.join(d, "slip.zip"), "wb") as f:
            f.write(buf.getvalue())
    write_case("zip_slip", build,
               {"slip.zip/ok.txt": sha256(b"ok\n"),
                "slip.zip/__/__/evil.txt": sha256(b"evil\n"),
                "slip.zip/dir/__/dotdot.txt": sha256(b"dd\n")})


def case_bad_crc():
    """对抗：bad.txt 本地数据区精确破坏（good.txt 应存活）"""
    def build(d):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("good.txt", b"good\n" * 100)
            zf.writestr("bad.txt", b"this will be corrupted\n" * 100)
        data = bytearray(buf.getvalue())
        # 精确定位 bad.txt 本地数据区（无 extra 字段：python 本地头 30B+文件名）
        zf = zipfile.ZipFile(io.BytesIO(bytes(data)))
        info = zf.getinfo("bad.txt")
        off = info.header_offset + 30 + len(info.filename.encode())
        csize = info.compress_size
        lo, hi = off + max(4, csize // 3), off + min(csize - 4, csize // 3 + 40)
        for i in range(lo, hi):   # 破坏 deflate 流中段（避开头尾结构）
            data[i] ^= 0xA5
        with open(os.path.join(d, "badcrc.zip"), "wb") as f:
            f.write(bytes(data))
    write_case("bad_crc", build, {"badcrc.zip/good.txt": sha256(b"good\n" * 100)})


def case_depth_bomb():
    """对抗：10 层 zip 链 > 默认深度 8 → 熔断退出码 3"""
    def build(d):
        cur = zip_bytes({"leaf.txt": b"bottom\n"})
        for i in range(9):
            cur = zip_bytes({f"l{i}.zip": cur})
        with open(os.path.join(d, "bomb.zip"), "wb") as f:
            f.write(cur)
    write_case("depth_bomb", build, {})


def case_missing_volume():
    """对抗：分片缺 .002 → 退出码 4"""
    inner = make_files({"m.txt": "missing volume test\n" * 2000})
    z = zip_bytes(inner)
    part = 16 * 1024
    while True:
        parts = [z[i:i + part] for i in range(0, len(z), part)]
        if len(parts) >= 4 or part <= 64:
            break
        part //= 2
    def build(d):
        for i, p in enumerate(parts):
            if i == 1:
                continue   # 缺 .002
            with open(os.path.join(d, f"data.zip.{i+1:03d}"), "wb") as f:
                f.write(p)
    write_case("missing_volume", build, {})


def case_bare_gz():
    """裸过滤器根：plain.txt.gz → outDir/plain.txt"""
    content = b"just a gzipped file\n" * 500
    def build(d):
        with open(os.path.join(d, "plain.txt.gz"), "wb") as f:
            f.write(gzip.compress(content))
    write_case("bare_gz", build, {"plain.txt": sha256(content)})


def case_zspan():
    """PKZIP spanning：data.z01 + data.zip（顺序陷阱 §3.3，拼接后逻辑名 data.zip）"""
    inner = make_files({"span.txt": "zspan order trap\n" * 3000})
    z = zip_bytes(inner)
    part = 64 * 1024
    parts = [z[i:i + part] for i in range(0, len(z), part)]
    if len(parts) < 3:
        part = 4096
        parts = [z[i:i + part] for i in range(0, len(z), part)]
    def build(d):
        for i in range(len(parts) - 1):
            with open(os.path.join(d, f"data.z{i+1:02d}"), "wb") as f:
                f.write(parts[i])
        with open(os.path.join(d, "data.zip"), "wb") as f:
            f.write(parts[-1])
    expected = {f"data.zip/{k}": v for k, v in tree_hash(inner).items()}
    write_case("zspan", build, expected)


def case_mixed_filters():
    """过滤链叠套：tar.bz2 → chain.tar.xz（tar 内容）+ blob 压缩文件条目"""
    deep = make_files({"deep/a.txt": "deepest\n" * 400})
    files = {"chain.tar.xz": lzma.compress(tar_bytes(deep)),
             "blob.bz2": bz2.compress(b"bz2 blob content\n" * 2000)}
    def build(d):
        with open(os.path.join(d, "mixed.tar.bz2"), "wb") as f:
            f.write(bz2.compress(tar_bytes(files)))
    expected = {f"mixed.tar.bz2/chain.tar.xz/{k}": v for k, v in tree_hash(deep).items()}
    expected["mixed.tar.bz2/blob"] = sha256(b"bz2 blob content\n" * 2000)
    write_case("mixed_filters", build, expected)


ALL = [
    case_plain_zip,
    case_three_layer,
    case_split_zip,
    case_split_entry_level,
    case_multimember_gz,
    case_two_passwords,
    case_7z_nested,
    case_zip_slip,
    case_bad_crc,
    case_depth_bomb,
    case_missing_volume,
    case_bare_gz,
    case_zspan,
    case_mixed_filters,
]

if __name__ == "__main__":
    os.makedirs(CASES_DIR, exist_ok=True)
    for fn in ALL:
        fn()
    print("[gen] 完成")
