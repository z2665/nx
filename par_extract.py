#!/usr/bin/env python
"""并行解压 zip（可从管道/内存流读取，无需中间文件）。

用法:
  python par_extract.py archive.zip 输出目录          # 从文件解压
  cat inner.zip | python par_extract.py - 输出目录    # 从 stdin 流式读取(全量进内存)

zip 中每个成员是独立压缩的, 多线程可并行解码; 配合管道可与外层解压并发执行。
"""
import io
import os
import shutil
import sys
import zipfile
from concurrent.futures import ThreadPoolExecutor

WORKERS = min(16, (os.cpu_count() or 4))


def main() -> None:
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)

    if src == "-":
        data = sys.stdin.buffer.read()
        make_zf = lambda: zipfile.ZipFile(io.BytesIO(data))
    else:
        make_zf = lambda: zipfile.ZipFile(src)

    with make_zf() as zf:
        members = [i for i in zf.infolist() if not i.is_dir()]

        # 每个线程持有独立的 ZipFile 视图, 避免共享底层文件对象的 seek 竞争
        zfs = [make_zf() for _ in range(WORKERS)]

        def extract(idx_info: tuple[int, zipfile.ZipInfo]) -> None:
            idx, info = idx_info
            dst = os.path.join(out, info.filename)
            os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
            with zfs[idx % WORKERS].open(info) as s, open(dst, "wb") as d:
                shutil.copyfileobj(s, d, 1 << 20)

        with ThreadPoolExecutor(max_workers=WORKERS) as ex:
            list(ex.map(extract, enumerate(members)))

        for z in zfs:
            z.close()


if __name__ == "__main__":
    main()
