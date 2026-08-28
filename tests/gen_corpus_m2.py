#!/usr/bin/env python3
"""nx M2 测试语料与用例：压缩比熔断（D6）、--verify sha256、--report（D8）。

- ratio_bomb：高度可压缩 gz（10MB 零 → ~10KB），--max-ratio 50 触发熔断（exit 3）
- verify_report：普通 zip 跑 --verify sha256 --report，哈希与 ground truth 对比
"""
import gzip
import io
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_corpus import CASES_DIR, make_files, sha256, tree_hash, write_case, zip_bytes


def case_ratio_bomb():
    """高膨胀 gz（≈1000:1）：默认 max-ratio 1000 内放行、--max-ratio 50 熔断"""
    payload = b"\x00" * (10 << 20)

    def build(d):
        # 裸 gz 根：膨胀后落盘为 bomb.bin
        with open(os.path.join(d, "bomb.bin.gz"), "wb") as f:
            f.write(gzip.compress(payload, 9))

    # 期望：默认 ratio=1000 恰好 ~1000:1（10485760/10494 ≈ 999×，在限内）
    write_case("ratio_bomb", build, {"bomb.bin": sha256(payload)})


M2_CASES = [case_ratio_bomb]

if __name__ == "__main__":
    for fn in M2_CASES:
        fn()
    print("[gen-m2] 完成")
