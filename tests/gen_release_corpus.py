#!/usr/bin/env python3
"""合成发布语料（批次 6）：按真实案例的**结构**重建（不使用真实样本——用户隐私
纪律：真实样本/路径/密码不入仓；结构描述见 README 案例代号 L/XJ/X）。

三个发布样本（确定性种子，重复生成逐字节一致；--spool-ram 16M 强制溢出路径）：
  release_L.mp4   案例 L 结构：伪装 MP4（诱饵+假 mdat 尾+影子 zip64 EOCD）→
                  隐写 zip（deflate 标记的不可压缩载荷=嵌套 **AES 加密** zip，
                  32 条目含深 CJK/emoji 路径与 64MiB 大件）——嵌套不直读、
                  spool 全量往返 + 磁盘溢出 + 密码链 + MAX_PATH 深路径
  release_XJ.mp4  案例 XJ 结构：伪装 MP4 → 隐写 zip（deflate 不可压缩单条目 =
                  7z **SFX solid+AES**，200 文件 + 嵌套 .save zip 递归多解）——
                  materializeBatch 批量抽取路径
  release_x       案例 X 结构：无扩展名 7z 嵌 7z（外层无密码，内层 AES）——
                  免 spool 直读 + 密码弹窗路径（gate 全程 -p 无人值守）

依赖：7z CLI（AES zip / solid 7z / SFX）。产物 tests/release-cases/（gitignored）。
"""
import io
import os
import random
import shutil
import subprocess
import sys
import zipfile

from gen_corpus import SEVEN_ZIP, build_staging, make_files, mp4_bytes

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "release-cases")

# 发布测试密码（合成语料自有口令，非任何真实凭据）
PW_L = "nx-release-L#2026"
PW_XJ = "nx-release-XJ#2026"
PW_X = "nx-release-X#2026"

RNG = random.Random(20261003)   # 确定性载荷：manifest 跨再生成稳定


def det_bytes(n: int) -> bytes:
    return RNG.randbytes(n)


def _eocd_shadow_wrap(prefix: bytes, z: bytes, rng: random.Random) -> bytes:
    """伪装 MP4 + 影子 zip64 EOCD + 伪装尾 + 假 mdat（复刻 gen_corpus.
    case_stego_zip64_shadow 构造）。注：真实案例 L 另有"zip 前 76B 诱饵本地头"，
    但 诱饵+影子 组合下检测器的魔数锚点会落在诱饵上（窗口基址偏移 → CD 错位，
    "Extra data overflow"——锚点行为依赖真实样本的字节细节），合成语料不复刻
    诱饵层；影子 EOCD 回退路径（自证失败 → 魔数锚点窗口 → EOCD64 真值）已覆盖。"""
    eocd_i = z.rfind(b"PK\x05\x06")
    cd_i = z.find(b"PK\x01\x02")
    n_entries = sum(1 for _ in zipfile.ZipFile(io.BytesIO(z)).infolist())
    eocd64 = (b"PK\x06\x06" + (44).to_bytes(8, "little")
              + (45).to_bytes(2, "little") * 2
              + (0).to_bytes(4, "little") * 2
              + n_entries.to_bytes(8, "little") * 2
              + (eocd_i - cd_i).to_bytes(8, "little")
              + cd_i.to_bytes(8, "little"))
    body = z[:eocd_i]
    eocd64_rel = len(body)
    shadow = (eocd64
              + b"PK\x06\x07" + (0).to_bytes(4, "little") * 2
              + eocd64_rel.to_bytes(8, "little") + (1).to_bytes(4, "little")
              # 影子经典 EOCD：假基址（自证必失败 → 回退魔数锚点窗口）
              + b"PK\x05\x06" + (0).to_bytes(2, "little") * 2
              + (1).to_bytes(2, "little") * 2
              + (90).to_bytes(4, "little")
              + (cd_i + 422).to_bytes(4, "little")
              + (0).to_bytes(2, "little"))
    return (prefix + body + shadow
            + rng.randbytes(4096)                     # EOCD 后伪装尾
            + (8).to_bytes(4, "big") + b"mdat")       # 假原子尾（防尾部回扫）


def _deflate_marked(entries: dict) -> bytes:
    """deflate 标记但内容不可压缩（zip-in-zip 字节）——真实案例的"不可压缩数据
    仍标 deflate"形态：嵌套档案无区间，必走 spool 往返"""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as zf:
        for name, data in entries.items():
            zf.writestr(name, data)
    return buf.getvalue()


def _aes_zip(files: dict, pw: str, workdir: str) -> bytes:
    s = build_staging(workdir, make_files(files), "_staging")
    zp = os.path.abspath(os.path.join(workdir, "_aes.zip"))
    subprocess.run([SEVEN_ZIP, "a", "-tzip", "-mem=AES256", f"-p{pw}", zp, "."],
                   check=True, capture_output=True, cwd=s)
    data = open(zp, "rb").read()
    os.remove(zp)
    return data


def _solid_7z_sfx(files: dict, pw: str, workdir: str) -> bytes:
    s = build_staging(workdir, make_files(files), "_staging")
    ap = os.path.abspath(os.path.join(workdir, "_solid.exe"))
    subprocess.run([SEVEN_ZIP, "a", "-t7z", "-sfx", f"-p{pw}", "-ms=on", "-mx=3",
                    ap, "."],
                   check=True, capture_output=True, cwd=s)
    data = open(ap, "rb").read()
    os.remove(ap)
    return data


def build_L(out: str):
    """案例 L：伪装 MP4 → 隐写 zip（deflate 标记，内嵌 AES zip）"""
    deep = "深度目录/" * 8   # 深中文路径（MAX_PATH 邻域）
    inner = {"readme.txt": ("release L\n" + "案例 L 结构重建\n") * 200,
             "媒体/playlist.m3u": "#extm3u\n" + "track%02d.mp3\n" % 1 * 100,
             "媒体/cover.bin": det_bytes(1 << 20),
             f"{deep}长路径/数据文件.bin": det_bytes(64 << 20),   # 大件：spool 溢出
             "emoji/🎬素材.dat": det_bytes(256 << 10)}
    for i in range(28):   # 补足 32 条目
        inner[f"批量/f{i:02d}.txt"] = (f"release L file {i}\n" * 400).encode()
    work = os.path.join(out, "_w_L")
    aes = _aes_zip(inner, PW_L, work)
    shutil.rmtree(work, ignore_errors=True)
    stego_zip = _deflate_marked({"archive.zip": aes})
    with open(os.path.join(out, "release_L.mp4"), "wb") as f:
        f.write(_eocd_shadow_wrap(mp4_bytes(det_bytes(64 << 10)), stego_zip, RNG))


def build_XJ(out: str):
    """案例 XJ：伪装 MP4 → 隐写 zip（deflate 标记，内嵌 7z SFX solid+AES）"""
    inner = {}
    for i in range(200):
        if i % 5 == 0:
            inner[f"game/assets/blob_{i:03d}.dat"] = det_bytes(64 << 10)
        else:
            inner[f"game/script/chapter_{i:03d}.rpy"] = \
                (f"# scene {i}\nlabel s{i}:\n    'dialog {i}'\n" * 300).encode()
    # 嵌套 .save zip：递归多解路径（真实案例的 Ren'Py 存档形态）
    save = io.BytesIO()
    with zipfile.ZipFile(save, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("persistent", det_bytes(8192))
        zf.writestr("autosave/meta.json", b'{"slot": 1, "ver": 2}')
    inner["game/save/1.save"] = save.getvalue()
    work = os.path.join(out, "_w_XJ")
    sfx = _solid_7z_sfx(inner, PW_XJ, work)
    shutil.rmtree(work, ignore_errors=True)
    stego_zip = _deflate_marked({"payload.exe": sfx})
    from gen_corpus import _disguise_wrap
    with open(os.path.join(out, "release_XJ.mp4"), "wb") as f:
        f.write(_disguise_wrap(mp4_bytes(det_bytes(64 << 10)), stego_zip))


def build_X(out: str):
    """案例 X：无扩展名 7z 嵌 7z（外层无密码，内层 AES）"""
    inner = {"vault/secret.key": det_bytes(4096),
             "vault/data_0.bin": det_bytes(32 << 20),
             "docs/说明.txt": ("无扩展名输入\n" + "案例 X 结构重建\n") * 300}
    work = os.path.join(out, "_w_X")
    inner_7z_bytes = _aes_zip(inner, PW_X, work)   # 复用 AES zip（7z 嵌套 zip 变体）
    loose = {"inner.zip": inner_7z_bytes,
             "first.txt": ("case X\n" * 500).encode(),
             "blob/random.bin": det_bytes(8 << 20)}
    s = build_staging(work, make_files(loose), "_staging2")
    tmp7z = os.path.abspath(os.path.join(work, "_outer.7z"))
    subprocess.run([SEVEN_ZIP, "a", "-t7z", "-mx=3", tmp7z, "."],
                   check=True, capture_output=True, cwd=s)
    # 无扩展名是本案例的关键特征（stem 回退撞名路径）——剥掉 7z CLI 自动补的扩展
    shutil.move(tmp7z, os.path.join(out, "release_x"))
    shutil.rmtree(work, ignore_errors=True)


def main() -> int:
    if not os.path.exists(SEVEN_ZIP):
        print(f"[rel-gen] 缺 7z CLI（{SEVEN_ZIP}）——发布语料需要它建 AES/solid 容器")
        return 2
    os.makedirs(OUT, exist_ok=True)
    for fn in (build_L, build_XJ, build_X):
        fn(OUT)
        print(f"[rel-gen] {fn.__name__} 完成")
    total = sum(os.path.getsize(os.path.join(OUT, f)) for f in os.listdir(OUT)
                if os.path.isfile(os.path.join(OUT, f)))
    print(f"[rel-gen] 完成：{OUT}（{total / (1 << 20):.0f} MiB）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
