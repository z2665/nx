#!/usr/bin/env python3
"""nx M1 测试语料生成器：zip / 7z / rar 三主流格式完整覆盖。

依赖：tests/tools/winrar/Rar.exe（WinRAR 试用版，仅生成语料用）+ 7z CLI。
在 gen_corpus.py（M0）之后运行；本脚本也复用其工具函数。
"""
import io
import os
import shutil
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_corpus import (CASES_DIR, SEVEN_ZIP, make_files, sha256, tree_hash,
                        write_case, build_staging, sevenz_available)


def rar_tool():
    exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools", "winrar", "Rar.exe")
    return exe if os.path.exists(exe) else None


def rar_available():
    return rar_tool() is not None


def rar_add(case_dir, out_name, staging, extra_args):
    out = os.path.abspath(os.path.join(case_dir, out_name))
    r = subprocess.run([rar_tool(), "a", *extra_args, "-r", out, "."],
                       cwd=staging, check=False, capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        raise RuntimeError(f"rar 失败: {r.stdout} {r.stderr}")


def stage(d, files, sub="_s"):
    return build_staging(d, files, sub)


# ---------------------------------------------------------------- 用例

def case_rar5_plain():
    if not rar_available():
        print("[gen] 跳过 rar5_plain（无 WinRAR）"); return
    files = make_files({"docs/readme.txt": "rar5 content\n" * 400,
                        "bin/blob.dat": os.urandom(120000)})
    def build(d):
        st = stage(d, files)
        rar_add(d, "data.rar", st, ["-ma5", "-idq"])
        shutil.rmtree(st)
    write_case("rar5_plain", build, {f"data.rar/{k}": v for k, v in tree_hash(files).items()})


def case_rar4_plain():
    if not rar_available():
        print("[gen] 跳过 rar4_plain（无 WinRAR）"); return
    files = make_files({"old/rar4.txt": "rar4 legacy format\n" * 300})
    def build(d):
        st = stage(d, files)
        rar_add(d, "legacy.rar", st, ["-ma4", "-idq"])
        shutil.rmtree(st)
    write_case("rar4_plain", build, {f"legacy.rar/{k}": v for k, v in tree_hash(files).items()})


def case_rar_solid():
    if not rar_available():
        print("[gen] 跳过 rar_solid（无 WinRAR）"); return
    files = make_files({f"s/f{i:02d}.txt": (f"solid block content {i}\n") * 200 for i in range(8)})
    def build(d):
        st = stage(d, files)
        rar_add(d, "solid.rar", st, ["-ma5", "-s", "-idq"])
        shutil.rmtree(st)
    write_case("rar_solid", build, {f"solid.rar/{k}": v for k, v in tree_hash(files).items()})


def case_rar_encrypted():
    """RAR5 头+内容全加密（-hp）：无密码连列表都拿不到（§6.1）"""
    if not rar_available():
        print("[gen] 跳过 rar_encrypted（无 WinRAR）"); return
    files = make_files({"secret/a.txt": "rar encrypted\n" * 300,
                        "b.bin": os.urandom(80000)})
    def build(d):
        st = stage(d, files)
        rar_add(d, "vault.rar", st, ["-ma5", "-hpRarPw@2024", "-idq"])
        shutil.rmtree(st)
    write_case("rar_encrypted", build, {f"vault.rar/{k}": v for k, v in tree_hash(files).items()})


def case_rar_multivol():
    """RAR 新式分卷：mv.part1.rar/.part2.rar/...（FS 级原生卷）"""
    if not rar_available():
        print("[gen] 跳过 rar_multivol（无 WinRAR）"); return
    files = make_files({"mv/data.bin": os.urandom(300000),
                        "mv/t.txt": "multivolume rar\n" * 500})
    def build(d):
        st = stage(d, files)
        rar_add(d, "mv.rar", st, ["-ma5", "-v100k", "-idq"])
        shutil.rmtree(st)
    write_case("rar_multivol", build, {f"mv.rar/{k}": v for k, v in tree_hash(files).items()})


def case_rar_oldvol():
    """RAR 旧式分卷：old.rar + old.r00/r01...（FS 级原生卷，旧命名）"""
    if not rar_available():
        print("[gen] 跳过 rar_oldvol（无 WinRAR）"); return
    files = make_files({"old/x.bin": os.urandom(200000)})
    def build(d):
        st = stage(d, files)
        rar_add(d, "old.rar", st, ["-ma4", "-v64k", "-vn", "-idq"])
        shutil.rmtree(st)
    write_case("rar_oldvol", build, {f"old.rar/{k}": v for k, v in tree_hash(files).items()})


def case_rar_entry_level_volumes():
    """条目级 RAR 原生分卷：outer.zip 内含 mv.part1.rar/.part2.rar（spool 卷窗口路径）"""
    if not rar_available():
        print("[gen] 跳过 rar_entry_level_volumes"); return
    files = make_files({"deep/inner.txt": "entry-level rar volumes\n" * 800,
                        "r.bin": os.urandom(150000)})
    def build(d):
        st = stage(d, files, "_inner")
        rar_add(d, "_mv.rar", st, ["-ma5", "-v100k", "-idq"])
        shutil.rmtree(st)
        vols = sorted(f for f in os.listdir(d) if f.startswith("_mv."))
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
            for v in vols:
                zf.writestr(v[1:], open(os.path.join(d, v), "rb").read())   # 去掉 staging 下划线
        for v in vols:
            os.remove(os.path.join(d, v))
        with open(os.path.join(d, "outer.zip"), "wb") as f:
            f.write(buf.getvalue())
    write_case("rar_entry_level_volumes", build,
               {f"outer.zip/mv.rar/{k}": v for k, v in tree_hash(files).items()})


def case_7z_encrypted():
    """7z AES 内容加密（头不加密：可列出但读不了）"""
    if not sevenz_available():
        print("[gen] 跳过 7z_encrypted"); return
    files = make_files({"enc/e.txt": "7z aes content\n" * 400,
                        "f.bin": os.urandom(100000)})
    def build(d):
        st = stage(d, files)
        subprocess.run([SEVEN_ZIP, "a", "-t7z", "-p7zPw@2024",
                        os.path.abspath(os.path.join(d, "sealed.7z")), "."],
                       check=True, capture_output=True, cwd=st)
        shutil.rmtree(st)
    write_case("7z_encrypted", build, {f"sealed.7z/{k}": v for k, v in tree_hash(files).items()})


def case_7z_mhe():
    """7z 头加密（-mhe=on）：无密码连条目列表都拿不到（§6.1）"""
    if not sevenz_available():
        print("[gen] 跳过 7z_mhe"); return
    files = make_files({"h/hidden.txt": "header encrypted 7z\n" * 300})
    def build(d):
        st = stage(d, files)
        subprocess.run([SEVEN_ZIP, "a", "-t7z", "-p7zPw@2024", "-mhe=on",
                        os.path.abspath(os.path.join(d, "blind.7z")), "."],
                       check=True, capture_output=True, cwd=st)
        shutil.rmtree(st)
    write_case("7z_mhe", build, {f"blind.7z/{k}": v for k, v in tree_hash(files).items()})


def case_7z_split():
    """7z 字节拼接分卷：sp.7z.001/.002/...（ConcatSource → spool → 7z.dll）"""
    if not sevenz_available():
        print("[gen] 跳过 7z_split"); return
    files = make_files({"sp/big.bin": os.urandom(200000), "sp/s.txt": "split 7z\n" * 200})
    def build(d):
        st = stage(d, files)
        subprocess.run([SEVEN_ZIP, "a", "-t7z", "-v64k",
                        os.path.abspath(os.path.join(d, "sp.7z")), "."],
                       check=True, capture_output=True, cwd=st)
        shutil.rmtree(st)
    write_case("7z_split", build, {f"sp.7z/{k}": v for k, v in tree_hash(files).items()})


def case_zip_sfx():
    """SFX zip：真 PE 头前缀 + zip 尾（流式失败 → D2 回退 spool+中央目录）"""
    files = make_files({"sfx/inside.txt": "sfx payload\n" * 300})
    def build(d):
        z = zip_bytes(files)
        exe_path = os.path.join(os.path.dirname(CASES_DIR), "..", "build", "nx.exe")
        prefix = open(exe_path, "rb").read()[:65536]
        with open(os.path.join(d, "installer.exe"), "wb") as f:
            f.write(prefix + z)
    write_case("zip_sfx", build, {f"installer.exe/{k}": v for k, v in tree_hash(files).items()})


def zip_bytes(files):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, data in files.items():
            zf.writestr(name, data)
    return buf.getvalue()


def case_triple_chain():
    """M1 招牌：zip → 7z(密码A) → rar(密码B) → 文件（三格式异密码嵌套链）"""
    if not rar_available() or not sevenz_available():
        print("[gen] 跳过 triple_chain"); return
    final = make_files({"final/report.txt": "triple chain final layer\n" * 300,
                        "final/d/data.bin": os.urandom(120000)})
    def build(d):
        st3 = stage(d, final, "_s3")
        rar_add(d, "_inner.rar", st3, ["-ma5", "-pRarChain@2024", "-idq"])
        shutil.rmtree(st3)
        st2 = stage(d, {"inner.rar": open(os.path.join(d, "_inner.rar"), "rb").read()}, "_s2")
        os.remove(os.path.join(d, "_inner.rar"))
        subprocess.run([SEVEN_ZIP, "a", "-t7z", "-p7zChain@2024",
                        os.path.abspath(os.path.join(d, "_mid.7z")), "."],
                       check=True, capture_output=True, cwd=st2)
        shutil.rmtree(st2)
        st1 = stage(d, {"mid.7z": open(os.path.join(d, "_mid.7z"), "rb").read()}, "_s1")
        os.remove(os.path.join(d, "_mid.7z"))
        with zipfile.ZipFile(os.path.join(d, "chain.zip"), "w", zipfile.ZIP_DEFLATED) as zf:
            zf.write(os.path.join(st1, "mid.7z"), "mid.7z")
        shutil.rmtree(st1)
    write_case("triple_chain", build,
               {f"chain.zip/mid.7z/inner.rar/{k}": v for k, v in tree_hash(final).items()})


ALL_M1 = [
    case_rar5_plain,
    case_rar4_plain,
    case_rar_solid,
    case_rar_encrypted,
    case_rar_multivol,
    case_rar_oldvol,
    case_rar_entry_level_volumes,
    case_7z_encrypted,
    case_7z_mhe,
    case_7z_split,
    case_zip_sfx,
    case_triple_chain,
]

if __name__ == "__main__":
    for fn in ALL_M1:
        try:
            fn()
        except FileNotFoundError:
            pass
        except RuntimeError as e:
            if "Unknown option" in str(e):
                print(f"[gen] 跳过 {fn.__name__}（WinRAR 7.x 不支持该开关）")
            else:
                raise
    print("[gen-m1] 完成")
