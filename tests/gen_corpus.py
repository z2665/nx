#!/usr/bin/env python3
"""nx 测试语料生成器（设计 §9.1）。

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
import random
import shutil
import struct
import subprocess
import sys
import tarfile
import zipfile
import zlib

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
    """3 层嵌套：data.tar.gz → inner.zip → 文件（出口标准：零中间文件）"""
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


def raw_zip(entries):
    """手搓最小 zip：条目名按**原始字节**写入（含 0x5C 反斜杠），绕开 python
    zipfile 在 Windows 的 os.sep 替换——混合分隔符穿越语料的唯一构造手段
    （红队 C1：该形态曾击穿只按 '/' 分割的消毒器，从输出根逃逸）"""
    out = io.BytesIO()
    central = io.BytesIO()
    for name, data in entries:
        nb = name if isinstance(name, bytes) else name.encode("utf-8")
        crc = zlib.crc32(data) & 0xFFFFFFFF
        off = out.tell()
        out.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, 0, 0, 0x4701,
                              crc, len(data), len(data), len(nb), 0))
        out.write(nb)
        out.write(data)
        central.write(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, 0, 0, 0x4701,
                                  crc, len(data), len(data), len(nb), 0, 0, 0, 0,
                                  0x20, off))
        central.write(nb)
    cd = central.getvalue()
    eocd = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(entries), len(entries),
                       len(cd), out.tell(), 0)
    return out.getvalue() + cd + eocd


def case_mixed_sep_zip():
    """对抗：混合分隔符穿越 zip（红队 C1 回归）——'\\' 与 '/' 同为段边界，
    穿越段进段级中和（.. → __）。手搓原始字节（python zipfile 在 Windows
    会把 0x5C 换成 '/'，库造不出该形态）"""
    def build(d):
        with open(os.path.join(d, "mixed.zip"), "wb") as f:
            f.write(raw_zip([
                (b"a/b\\..\\..\\..\\..\\ESC.txt", b"escaped\n"),
                (b"..\\..\\bs.txt", b"bs\n"),
                (b"dir\\\\file.txt", b"dbl\n"),
                (b"ok.txt", b"ok\n"),
            ]))
    write_case("mixed_sep_zip", build,
               {"mixed.zip/a/b/__/__/__/__/ESC.txt": sha256(b"escaped\n"),
                "mixed.zip/__/__/bs.txt": sha256(b"bs\n"),
                "mixed.zip/dir/file.txt": sha256(b"dbl\n"),
                "mixed.zip/ok.txt": sha256(b"ok\n")})


def case_mixed_sep_tar():
    """对抗：混合分隔符穿越 tar（红队 C1 回归）——tar reader 无分隔符规范化，
    反斜杠穿越段直达公共落盘层；tarfile 忠实保留 0x5C，可直接库造"""
    def build(d):
        with open(os.path.join(d, "mixed.tar"), "wb") as f:
            f.write(tar_bytes({"a/b\\..\\..\\TESC.txt": b"tar-escaped\n",
                               "ok_t.txt": b"ok\n"}))
    write_case("mixed_sep_tar", build,
               {"mixed.tar/a/b/__/__/TESC.txt": sha256(b"tar-escaped\n"),
                "mixed.tar/ok_t.txt": sha256(b"ok\n")})


def case_empty_entries():
    """全空条目容器（红队 M5 回归）：probe 轮耗尽迭代到 EOF 后，主迭代
    不得在 libarchive eof 态上二次 next_header（曾报 INTERNAL ERROR 且
    exit 1——合法空 zip 被判部分失败）"""
    def build(d):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("sub/", b"")
            zf.writestr("placeholder.bin", b"")
        with open(os.path.join(d, "empty.zip"), "wb") as f:
            f.write(buf.getvalue())
    write_case("empty_entries", build,
               {"empty.zip/placeholder.bin": sha256(b"")})


def case_spool_cap():
    """spool 磁盘溢出总量熔断（红队 M6 回归）：zip-in-zip 的中间字节不进任何
    输出侧预算，溢出路径曾无上界无水位。两个输入共享一套断言（run_tests）：
    spoolbomb.zip：外层 deflate 载荷=不可压缩内层 zip（必走 spool 全量往返）
      ① --max-bytes 1MiB + --spool-ram 64KiB → 溢出相位超限 exit 3
      ② 仅 --spool-ram 64KiB → 默认上限放行且树吻合（熔断不误伤正常溢出）
    spooldiskbomb.zip：内层 stored 诱饵条目（zip 头+全零，外层把 2MiB 压到
    KB 级，子打开必败、最终零输出）——修复前 spool 全量落盘后才报损坏
    exit 1，修复后溢出即熔断 exit 3（差异即回归点）"""
    inner = {"big.bin": random.Random(20261005).randbytes(2 << 20)}
    ib = zip_bytes(inner)
    decoy = b"PK\x03\x04" + bytes(2 << 20)   # zip 头 + 全零：可被外层压缩的假子档
    dbuf = io.BytesIO()
    with zipfile.ZipFile(dbuf, "w", zipfile.ZIP_STORED) as zf:
        zf.writestr("broken.zip", decoy)
    decoy_inner = dbuf.getvalue()
    def build(d):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("archive.zip", ib)
        with open(os.path.join(d, "spoolbomb.zip"), "wb") as f:
            f.write(buf.getvalue())
        buf2 = io.BytesIO()
        with zipfile.ZipFile(buf2, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("archive.zip", decoy_inner)
        with open(os.path.join(d, "spooldiskbomb.zip"), "wb") as f:
            f.write(buf2.getvalue())
    write_case("spool_cap", build,
               {f"spoolbomb.zip/archive.zip/{k}": v for k, v in tree_hash(inner).items()})


def case_long_path():
    """超长输出路径（MAX_PATH）：rel 271 字符 → 测试机输出根下最终路径 >320，
    叠加 .nxpart- 临时名后缀稳超 260。Sink 的 CreateFileW/MoveFileExW 必须经
    \\?\ 前缀——裸路径 >260 报 ERROR_PATH_NOT_FOUND 而非"路径过长"
    （15GB 隐写 MP4 真实案例：目录全建成、仅最深文件落盘失败）"""
    seg = "d" * 28
    rel = "/".join([seg] * 8) + "/" + "f" * 36 + ".bin"
    payload = b"deep path payload"
    def build(d):
        with zipfile.ZipFile(os.path.join(d, "longpath.zip"), "w", zipfile.ZIP_STORED) as zf:
            zf.writestr(rel, payload)
            zf.writestr("short.txt", b"ok\n")
    write_case("long_path", build,
               {f"longpath.zip/{rel}": sha256(payload),
                "longpath.zip/short.txt": sha256(b"ok\n")})


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
    """对抗：12 层 zip 链 > 默认深度 10（决策 D-1：8→10）→ 熔断退出码 3"""
    def build(d):
        cur = zip_bytes({"leaf.txt": b"bottom\n"})
        for i in range(11):
            cur = zip_bytes({f"l{i}.zip": cur})
        with open(os.path.join(d, "bomb.zip"), "wb") as f:
            f.write(cur)
    write_case("depth_bomb", build, {})


def case_filter_depth_bomb():
    """对抗：30 层嵌套 gzip > 过滤器链深度上限（决策 D-1：过滤器链纳入 --depth，
    默认 10）→ 熔断退出码 3。修复前过滤器分支同 depth 无限递归（4MiB 输入可
    构造数千层嵌套 gzip → 栈溢出/线程耗尽，DoS 面）"""
    def build(d):
        cur = gzip.compress(b"filter chain bomb\n" * 64)
        for _ in range(29):
            cur = gzip.compress(cur)
        with open(os.path.join(d, "bomb.gz"), "wb") as f:
            f.write(cur)
    write_case("filter_depth_bomb", build, {})


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


def case_pw_retry_nested():
    """密码失败重试（D6 触发族 1 回归）：嵌套加密 zip 前置目录条目 + 首轮错密码。
    probe 先把目录条目存入重放队列、再在首数据条目密码验证失败——修复前队列里
    的条目源持读取器回指成自引用环，失败读取器永不析构（泄漏读取器+spool+视图，
    15GB 临时文件残留案例根因）。结构上等价于 build-diag 校准语料。
    依赖 7z CLI（AES zip）；候选顺序 [错, 对] 覆盖失败重试路径。"""
    if not sevenz_available():
        print("[gen] 跳过 pw_retry_nested（无 7z CLI）")
        return
    inner = make_files({"docs/readme.txt": "retry me\n" * 200,
                        "data/blob.bin": os.urandom(120000)})

    def build(d):
        s = build_staging(d, inner, "_pr")
        zp = os.path.abspath(os.path.join(d, "_vault.zip"))
        # 压目录本身：7z 会把 docs/、data/ 目录条目写在最前（probe 队列非空的关键）
        subprocess.run([SEVEN_ZIP, "a", "-tzip", "-mem=AES256", "-pRetryPw@2026", zp,
                        "."], check=True, capture_output=True, cwd=s)
        with open(zp, "rb") as f:
            v = f.read()
        os.remove(zp)
        with open(os.path.join(d, "outer.tar.gz"), "wb") as f:
            f.write(gzip.compress(tar_bytes({"vault.zip": v})))
    expected = {f"outer.tar.gz/vault.zip/{k}": v for k, v in tree_hash(inner).items()}
    write_case("pw_retry_nested", build, expected)


def case_sibling_pw_cache():
    """密码缓存键语义（设计 §6.2 层身份）：不同父容器下的同名分片组——a.tar.gz 与
    b.tar.gz 各含 data.zip.001+（异密码，候选只给 b 的）。修复前缓存键 = 深度+名
    （"第 3 层 data.zip"），a 组耗尽候选把共享游标推过界 → b 组连候选都不试即假性
    PasswordExhausted（实测 0 文件解出）；逻辑路径键（outer/a.tar.gz/data.zip 与
    …/b.tar.gz/…）区分兄弟分支后 b 组正常解开、a 组如常报缺密码（退出码 2）。
    依赖 7z CLI（AES zip）。"""
    if not sevenz_available():
        print("[gen] 跳过 sibling_pw_cache（无 7z CLI）")
        return

    def tarz(files):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as tf:
            for name, data in files.items():
                ti = tarfile.TarInfo(name)
                ti.size = len(data)
                tf.addfile(ti, io.BytesIO(data))
        return gzip.compress(buf.getvalue())

    def build(d):
        inner = {}
        for parent, pw in (("a.tar.gz", "SibA@2026"), ("b.tar.gz", "SibB@2026")):
            stage = build_staging(d, {"deep.txt": b"payload\n" * 400}, "_spc_" + parent)
            zp = os.path.abspath(os.path.join(d, "_spc.zip"))
            subprocess.run([SEVEN_ZIP, "a", "-tzip", "-mem=AES256", "-p" + pw, zp, "."],
                           check=True, capture_output=True, cwd=stage)
            z = open(zp, "rb").read()
            os.remove(zp)
            part = 32 * 1024
            parts = [z[i:i + part] for i in range(0, len(z), part)]
            inner[parent] = tarz({f"data.zip.{i+1:03d}": p for i, p in enumerate(parts)})
        with open(os.path.join(d, "outer.tar.gz"), "wb") as f:
            f.write(tarz(inner))
    # 候选只给 b 的密码：a 组如常缺密码（exit 2），b 组必须解开
    write_case("sibling_pw_cache", build,
               {"outer.tar.gz/b.tar.gz/data.zip/deep.txt":
                    sha256(b"payload\n" * 400)})


def case_bare_gz():
    """裸过滤器根：plain.txt.gz → outDir/plain.txt"""
    content = b"just a gzipped file\n" * 500
    def build(d):
        with open(os.path.join(d, "plain.txt.gz"), "wb") as f:
            f.write(gzip.compress(content))
    write_case("bare_gz", build, {"plain.txt": sha256(content)})


def case_bare_zstd():
    """裸 zstd 过滤器根（评审 spec m5：zstd 此前仅有引擎能力无端到端语料；
    python 3.14 起标准库 compression.zstd 可造——lz4/.Z 无标准库渠道，仍缺席）"""
    from compression import zstd
    content = b"just a zstandard file\n" * 500
    def build(d):
        with open(os.path.join(d, "plain.txt.zst"), "wb") as f:
            f.write(zstd.compress(content))
    write_case("bare_zstd", build, {"plain.txt": sha256(content)})


def case_dup_many():
    """海量同名条目（红队 m1 锚）：重名编号 2..N 连续且内容一一对应——
    dedupe 游标推进的编号序与线性重试逐一同构（曾 O(N²)：5000 条 13.2s）"""
    n = 1500
    payload = b"duplicate name payload\n"
    expected = {"dup.txt": sha256(payload)}
    for i in range(2, n + 1):
        expected[f"dup ({i}).txt"] = sha256(payload)
    def build(d):
        with zipfile.ZipFile(os.path.join(d, "dupmany.zip"), "w", zipfile.ZIP_STORED) as zf:
            for _ in range(n):
                zf.writestr("dup.txt", payload)
    write_case("dup_many", build, expected)



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


# ---------------------------------------------------------------- 隐写（--stego）

def mp4_atom(typ: bytes, payload: bytes) -> bytes:
    return (8 + len(payload)).to_bytes(4, "big") + typ + payload


def mp4_bytes(mdat_payload: bytes, mdat_size_zero: bool = False) -> bytes:
    """最小合法 MP4：ftyp + mdat + free（原子精确收尾 = 干净文件）。"""
    ftyp = mp4_atom(b"ftyp", b"\x00\x00\x02\x00isomiso2mp41")
    if mdat_size_zero:
        mdat = b"\x00\x00\x00\x00" + b"mdat" + mdat_payload   # size=0 = 延伸到 EOF
    else:
        mdat = mp4_atom(b"mdat", mdat_payload)
    return ftyp + mdat + mp4_atom(b"free", b"")


def case_stego_mp4_zip():
    inner = make_files({"flag.txt": "hidden message\n" * 50,
                        "docs/plan.txt": "stego corpus\n" * 100})
    z = zip_bytes(inner)

    def build(d):
        with open(os.path.join(d, "video.mp4"), "wb") as f:
            f.write(mp4_bytes(b"\x00" * 4096))
            f.write(z)
    write_case("stego_mp4_zip", build,
               {f"video.mp4/{k}": v for k, v in tree_hash(inner).items()})


def case_stego_jpg_zip():
    # 非 MP4 多合一：JPEG 头 + zip（EOCD 反扫路径；无 atom 结构可用）
    inner = make_files({"a.txt": "polyglot\n" * 80})
    z = zip_bytes(inner)

    def build(d):
        with open(os.path.join(d, "photo.jpg"), "wb") as f:
            f.write(b"\xff\xd8\xff\xe0\x00\x10JFIF\x00" + b"\x00" * 2048)
            f.write(z)
    write_case("stego_jpg_zip", build,
               {f"photo.jpg/{k}": v for k, v in tree_hash(inner).items()})


def case_stego_mp4_mdat0_zip():
    # mdat size=0：原子步进判"无尾部"，EOCD 反扫兜住尾接 zip
    inner = make_files({"note.txt": "inside mdat\n" * 40})
    z = zip_bytes(inner)

    def build(d):
        with open(os.path.join(d, "clip.mp4"), "wb") as f:
            f.write(mp4_bytes(b"\x00" * 512, mdat_size_zero=True))
            f.write(z)
    write_case("stego_mp4_mdat0_zip", build,
               {f"clip.mp4/{k}": v for k, v in tree_hash(inner).items()})


def case_stego_none():
    # 干净 MP4（原子精确收尾）：--stego → 未检测到（exit 0，零输出）
    def build(d):
        with open(os.path.join(d, "clean.mp4"), "wb") as f:
            f.write(mp4_bytes(b"\x00" * 8192))
    write_case("stego_none", build, {})


def _stego_reuse(src_case, src_file, out_name, out_case):
    """复用既有 m1 语料做 7z/rar 尾部隐写（缺失则跳过——run_tests 侧同步跳过）。"""
    src = os.path.join(CASES_DIR, src_case, src_file)
    if not os.path.exists(src):
        print(f"[gen] {out_case}: 跳过（缺 {src_case}/{src_file}，先跑 gen_corpus_m1.py）")
        return
    exp = json.load(open(os.path.join(CASES_DIR, src_case, "expected.json"),
                         encoding="utf-8"))["files"]

    def build(d):
        with open(os.path.join(d, out_name), "wb") as f:
            f.write(mp4_bytes(b"\x00" * 1024))
            f.write(open(src, "rb").read())
    expected = {f"{out_name}/{k.split('/', 1)[1]}": v
                for k, v in exp.items()}
    write_case(out_case, build, expected)


def case_stego_mp4_rar():
    _stego_reuse("rar5_plain", "data.rar", "movie.mp4", "stego_mp4_rar")


def case_stego_mp4_7z():
    _stego_reuse("7z_encrypted", "sealed.7z", "film.mp4", "stego_mp4_7z")


def _disguise_wrap(mp4_prefix: bytes, zip_bytes_: bytes) -> bytes:
    """复刻真实伪装样本（D:\\...\\1.mp4）三件套：
    ① zip 前诱饵本地头（首见 PK 偏移 ≠ EOCD 反推基址）
    ② EOCD 之后拖随机伪装数据
    ③ 文件末尾补一个 size=8 的假 mdat 原子头（专防尾部回扫类检测）"""
    return (mp4_prefix
            + b"PK\x03\x04\x2d\x00\x00\x08\x08\x00" + b"\xde\xad" * 32   # 诱饵：76B
            + zip_bytes_
            + os.urandom(4096)                                            # EOCD 后伪装
            + (8).to_bytes(4, "big") + b"mdat")                           # 假原子尾


def case_nested_zip_stored():
    """外层 STORED 内嵌 inner.zip——免 spool 窗口直读正路径（嵌套容器直读语料）。
    外层必须 stored：deflate 条目载荷在父中无连续区间，走 spool 回退路径。"""
    inner = make_files({"直接读/inner1.txt": ("region direct" + chr(10)) * 300,
                        "直接读/d2/inner2.bin": os.urandom(60000),
                        # >2MiB：越过 libarchive read-ahead 缓冲（256KB），确保
                        # region 直读命中路径被覆盖（小文件整包缓冲时走 spool 回退）
                        "直接读/big.bin": os.urandom(2 << 20)})
    z2 = zip_bytes(inner)
    loose = ("loose" + chr(10)) * 50

    def build(d):
        with zipfile.ZipFile(os.path.join(d, "outer.zip"), "w", zipfile.ZIP_STORED) as z:
            z.writestr("inner.zip", z2)          # 原样字节（stored）
            z.writestr("loose.txt", loose)
    write_case("nested_zip_stored", build,
               {"outer.zip/inner.zip/直接读/inner1.txt":
                    tree_hash(inner)["直接读/inner1.txt"],
                "outer.zip/inner.zip/直接读/d2/inner2.bin":
                    tree_hash(inner)["直接读/d2/inner2.bin"],
                "outer.zip/inner.zip/直接读/big.bin":
                    tree_hash(inner)["直接读/big.bin"],
                "outer.zip/loose.txt": sha256(loose.encode())})


def case_region_decoy():
    """区间推导诱饵回归（regionOf 头自证，P0 修复）：外层 deflate 条目的载荷是
    一个首条目为 stored 目录条目（method0、csize=usize=0）的内层 zip——载荷近乎
    不可压缩，外层 deflate 退化为 stored 块，内层首条目头原样进压缩流且落在数据
    相位回溯窗口内。修复前 regionOf 无头自证：把它当本条目的 stored 本地头，推
    出"内层去掉前若干字节"的错位区间，子打开后 CD 偏移全错中途读头失败（真实
    形态：嵌套 AES zip 的目录条目；触发随流字节巧合漂移，曾致 CI 发布门环境
    依赖性失败）。期望：本地头自证（csize==usize==条目尺寸）识破 → 回退 spool
    正确解出。"""
    rng = random.Random(20261004)   # 跨再生成确定（区别于 os.urandom 用例）
    data = rng.randbytes(320 * 1024)
    ib = io.BytesIO()
    with zipfile.ZipFile(ib, "w") as zf:
        di = zipfile.ZipInfo("d/")
        di.compress_type = zipfile.ZIP_STORED
        zf.writestr(di, b"")                    # stored 目录条目 = 诱饵头
        fi = zipfile.ZipInfo("d/data.bin")
        fi.compress_type = zipfile.ZIP_STORED
        zf.writestr(fi, data)
    payload = ib.getvalue()

    def build(d):
        with zipfile.ZipFile(os.path.join(d, "outer.zip"), "w",
                             zipfile.ZIP_DEFLATED) as z:
            z.writestr("archive.zip", payload)  # 不可压缩 → stored 块，诱饵保真
            z.writestr("top.txt", "decoy case" + chr(10))
    write_case("region_decoy", build,
               {"outer.zip/archive.zip/d/data.bin": sha256(data),
                "outer.zip/top.txt": sha256("decoy case\n".encode())})


def case_stego_disguise():
    inner = make_files({"payload/game.exe": "disguised\n" * 200,
                        "docs/manual.txt": "trailer junk beyond EOCD\n" * 40})
    z = zip_bytes(inner)

    def build(d):
        with open(os.path.join(d, "trap.mp4"), "wb") as f:
            f.write(_disguise_wrap(mp4_bytes(b"\x00" * 2048), z))
    write_case("stego_disguise", build,
               {f"trap.mp4/{k}": v for k, v in tree_hash(inner).items()})


def case_stego_disguise_pw():
    # 加密变体（真实样本同款：AES zip + 伪装尾）；无 7z CLI 则跳过
    if not sevenz_available():
        print("[gen] 跳过 stego_disguise_pw（无 7z CLI）")
        return
    inner = make_files({"vault/key.dat": os.urandom(65536),
                        "readme.txt": "encrypted stego\n" * 60})
    th = tree_hash(inner)

    def build(d):
        s1 = build_staging(d, inner, "_sd")
        zp = os.path.abspath(os.path.join(d, "_inner.zip"))
        subprocess.run([SEVEN_ZIP, "a", "-tzip", "-mem=AES256", "-pStegoPw@2024",
                        zp, "."], check=True, capture_output=True, cwd=s1)
        with open(os.path.join(d, "vault.mp4"), "wb") as f:
            f.write(_disguise_wrap(mp4_bytes(b"\x00" * 2048),
                                   open(zp, "rb").read()))
        os.remove(zp)
    write_case("stego_disguise_pw", build, {f"vault.mp4/{k}": v for k, v in th.items()})


def case_stego_zip64_shadow():
    """zip64 影子 EOCD（复刻真实样本 D:\\…\\1.mp4）：真值在 EOCD64+定位器，
    经典 EOCD 的 cdOffset/cdSize/条目数是错的，且 EOCD 之后还有伪装尾巴。
    期望：EOCD 数学自证（CD 首 PK\x01\x02）失败 → 魔数锚点窗口到 EOF →
    libarchive 依 EOCD64 真值解开。"""
    inner = make_files({"z64/a.bin": os.urandom(50000),
                        "z64/b.txt": "shadow eocd\n" * 80})
    th = tree_hash(inner)
    z = zip_bytes(inner)
    eocd_i = z.rfind(b"PK\x05\x06")
    cd_i = z.find(b"PK\x01\x02")
    n = len(inner)
    eocd64 = (b"PK\x06\x06" + (44).to_bytes(8, "little")
              + (45).to_bytes(2, "little") + (45).to_bytes(2, "little")
              + (0).to_bytes(4, "little") * 2
              + n.to_bytes(8, "little") * 2
              + (eocd_i - cd_i).to_bytes(8, "little")
              + cd_i.to_bytes(8, "little"))

    def build(d):
        prefix = mp4_bytes(b"\x00" * 1024)
        with open(os.path.join(d, "ghost.mp4"), "wb") as f:
            f.write(prefix)
            f.write(z[:eocd_i])                      # 本地头+数据+CD（去掉旧 EOCD）
            eocd64_rel = f.tell() - len(prefix)      # 定位器存 zip 相对偏移
            f.write(eocd64)
            f.write(b"PK\x06\x07" + (0).to_bytes(4, "little") * 2
                    + eocd64_rel.to_bytes(8, "little") + (1).to_bytes(4, "little"))
            # 影子经典 EOCD：cdOffset 偏 +422、cdSize=90、条目=1（自证必失败）
            f.write(b"PK\x05\x06" + (0).to_bytes(2, "little") * 2
                    + (1).to_bytes(2, "little") * 2
                    + (90).to_bytes(4, "little")
                    + (cd_i + 422).to_bytes(4, "little")
                    + (0).to_bytes(2, "little"))
            f.write(os.urandom(2048))                # EOCD 后伪装尾巴
            f.write((8).to_bytes(4, "big") + b"mdat")
    write_case("stego_zip64_shadow", build, {f"ghost.mp4/{k}": v for k, v in th.items()})


ALL = [
    case_plain_zip,
    case_three_layer,
    case_split_zip,
    case_split_entry_level,
    case_multimember_gz,
    case_two_passwords,
    case_7z_nested,
    case_zip_slip,
    case_mixed_sep_zip,
    case_mixed_sep_tar,
    case_empty_entries,
    case_spool_cap,
    case_long_path,
    case_bad_crc,
    case_depth_bomb,
    case_filter_depth_bomb,
    case_pw_retry_nested,
    case_sibling_pw_cache,
    case_missing_volume,
    case_bare_gz,
    case_bare_zstd,
    case_dup_many,
    case_zspan,
    case_mixed_filters,
    case_stego_mp4_zip,
    case_stego_jpg_zip,
    case_stego_mp4_mdat0_zip,
    case_stego_none,
    case_stego_mp4_rar,
    case_stego_mp4_7z,
    case_stego_disguise,
    case_stego_disguise_pw,
    case_stego_zip64_shadow,
    case_nested_zip_stored,
    case_region_decoy,
]

if __name__ == "__main__":
    os.makedirs(CASES_DIR, exist_ok=True)
    for fn in ALL:
        fn()
    print("[gen] 完成")
