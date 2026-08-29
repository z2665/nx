#!/usr/bin/env python3
"""nx 文件名编码测试语料：无 EFS 标志的 CP932(日)/GBK(中) 原始字节名 zip。

复现真实缺陷（D:\\...\\2.zip 案例）：829 个日文名条目变 "_"。
zipfile 不支持 bytes 名 → 手写 zip 结构（raw deflate、无 EFS 位）。
"""
import os
import struct
import sys
import tempfile
import zlib
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_corpus import CASES_DIR, write_case


def raw_deflate(data):
    c = zlib.compressobj(6, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def mk_zip(path, entries):
    out = open(path, "wb")
    central = b""
    for name, data in entries:
        off = out.tell()
        comp = raw_deflate(data)
        crc = zlib.crc32(data) & 0xFFFFFFFF
        out.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, 8, 0, 0, crc,
                              len(comp), len(data), len(name), 0) + name + comp)
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, 8, 0, 0,
                               crc, len(comp), len(data), len(name), 0, 0, 0, 0, 0,
                               off) + name
    coff = out.tell()
    out.write(central)
    out.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(entries), len(entries),
                          len(central), coff, 0))
    out.close()


def _case(name, entries, rels):
    import hashlib
    def build(d):
        mk_zip(os.path.join(d, "cpnames.zip"), entries)
    exp = {}
    for (n, data), rel in zip(entries, rels):
        exp[rel] = hashlib.sha256(data).hexdigest()
    write_case(name, build, exp)


def case_cp_names_jp():
    """单语言归档（真实形态）：CP932 日文名"""
    _case("cp_names_jp", [
        (b"www/bgm/" + "うさダンス.ogg".encode("cp932"), b"japanese-audio-data"),
        (b"www/bgm/" + "プレッサー_エロシーン1.rpgmvo".encode("cp932"), b"japanese-presser"),
        (b"www/plain.dat", b"ascii-ok"),
    ], ["cpnames.zip/www/bgm/うさダンス.ogg",
       "cpnames.zip/www/bgm/プレッサー_エロシーン1.rpgmvo",
       "cpnames.zip/www/plain.dat"])


def case_cp_names_cn():
    """单语言归档：GBK 中文名"""
    _case("cp_names_cn", [
        (b"www/txt/" + "攻略手册.txt".encode("gbk"), b"chinese-gbk-data"),
        ("docs/新建文件夹/说明.txt".encode("gbk"), b"chinese-nested"),
        (b"www/plain.dat", b"ascii-ok"),
    ], ["cpnames.zip/www/txt/攻略手册.txt",
       "cpnames.zip/docs/新建文件夹/说明.txt",
       "cpnames.zip/www/plain.dat"])


if __name__ == "__main__":
    case_cp_names_jp()
    case_cp_names_cn()
    zi = zipfile.ZipFile(os.path.join(CASES_DIR, "cp_names_jp", "cpnames.zip"))
    assert len(zi.infolist()) == 3
    print("[gen-fn] 完成")
