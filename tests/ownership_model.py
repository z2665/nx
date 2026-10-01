#!/usr/bin/env python3
"""nx 所有权小模型（roadmap §7.4 批次 4 前置验收门的可执行形态）。

有穷模型穷举检查（对象 ≤6、操作 ≤10、深度 ≤12），替代外部 Alloy 工具——
验收标准不变：**修复前语义必须能复现 replayQ_ 泄漏反例；weak_ptr+KeepAlive
方案必须全序列无反例**。三个变体：

  legacy    修复前：probe 把条目源（持读者强回指）存进读者自己的重放队列
  weakOnly  只有 weak_ptr 没有 KeepAlive（roadmap 注记"两者必须配套"的反面）
  fixed     weak_ptr + Sink 任务持 KeepAlive 令牌

对象：R=LaSeqReader，E=EntrySource，S=SpoolBuffer，V=View，T=SinkTask，K=KeepAlive
不变式：
  NoLeak          强边图无环（等价：无"活着但从根不可达"的对象——模型里 GC
                  即刻回收零入度对象，不可达当活 ⇔ 成环）
  AsyncNoUseAfterDead  异步写出任务存活期间其条目源的读者必须存活
"""
import sys
from itertools import count

ROOT = "ROOT"
OBJS = ["R", "E", "S", "V", "T", "K"]
MAX_DEPTH = 12


def canon(alive, strong, weak, roots):
    return (
        frozenset(alive),
        # ROOT 是恒活伪节点：其出边必须保留，否则"根持有/根已释放"两状态会
        # 被错误去重合并（泄漏反例因此探测不到）
        frozenset((a, b) for a, b in strong
                  if (a == ROOT or a in alive) and b in alive),
        frozenset((a, b) for a, b in weak if a in alive and b in alive),
        frozenset(roots),
    )


def gc(alive, strong, weak):
    """即刻回收：反复删除强入度为 0 的活对象（ROOT 恒活）。"""
    changed = True
    alive = set(alive)
    while changed:
        changed = False
        for o in list(alive):
            indeg = sum(1 for (a, b) in strong
                        if b == o and (a == ROOT or a in alive))
            if indeg == 0:
                alive.discard(o)
                strong = {(a, b) for (a, b) in strong if a != o and b != o}
                weak = {(a, b) for (a, b) in weak if a != o and b != o}
                changed = True
    return alive, strong, weak


def ops(variant):
    """返回 (名字, 前置条件, 状态迁移) 列表。状态 = (alive, strong, weak, roots)。"""
    def open_(st):
        alive, strong, weak, roots = st
        if "R" in alive:
            return None
        alive = set(alive) | {"R", "S", "V"}
        strong = set(strong) | {("ROOT", "R"), ("R", "S"), ("R", "V"), ("V", "S")}
        return gc(alive, strong, weak) + (roots,)

    def probe_push(st):
        # probe 预取：读者把条目存入自己的重放队列。
        # legacy：队列里的条目源持 R 的强回指（replayQ_ 环的成因）；
        # fixed：重放队列只存元数据（ReplayRecord），条目源到 next() 才诞生且只持弱引用
        alive, strong, weak, roots = st
        if "R" not in alive:
            return None
        if variant == "legacy":
            alive = set(alive) | {"E"}
            strong = set(strong) | {("R", "E"), ("E", "R")}
            return gc(alive, strong, weak) + (roots,)
        return st   # fixed/weakOnly：无堆事件

    def next_entry(st):
        # next() 重放/新条目：条目源诞生，被调用方（walk 栈，经 ROOT 表意）持有
        alive, strong, weak, roots = st
        if "R" not in alive or "E" in alive:
            return None
        alive = set(alive) | {"E"}
        strong = set(strong) | {("ROOT", "E")}
        if variant == "legacy":
            strong = strong | {("E", "R")}
        else:
            weak = set(weak) | {("E", "R")}
        return gc(alive, strong, weak) + (roots,)

    def fail_open(st):
        # 触发族 1：打开失败（加密嵌套 zip 无密码首轮），读者被丢弃
        alive, strong, weak, roots = st
        if "R" not in alive:
            return None
        strong = {(a, b) for (a, b) in strong if not (a == "ROOT" and b == "R")}
        return gc(alive, strong, weak) + (roots,)

    def abandon(st):
        # 触发族 2：成功打开但中途弃置（取消/分支错误）
        return fail_open(st)

    def emit_independent(st):
        # 独立源 → Sink 线程池异步写：任务持条目源；
        # fixed：任务另持 KeepAlive 令牌（读者别名构造的 shared_ptr<void>）。
        # 提交时序约束：emit 发生在读者存活期间（walker 帧内），任务可超出帧存活
        alive, strong, weak, roots = st
        if "E" not in alive or "T" in alive or "R" not in alive:
            return None
        alive = set(alive) | {"T"}
        strong = set(strong) | {("ROOT", "T"), ("T", "E")}
        if variant == "fixed":
            alive = alive | {"K"}
            strong = strong | {("T", "K"), ("K", "R")}
        return gc(alive, strong, weak) + (roots,)

    def run_task(st):
        alive, strong, weak, roots = st
        if "T" not in alive:
            return None
        strong = {(a, b) for (a, b) in strong if not (a == "ROOT" and b == "T")}
        return gc(alive, strong, weak) + (roots,)

    return [
        ("open", open_),
        ("probePush", probe_push),
        ("next", next_entry),
        ("failOpen", fail_open),
        ("abandon", abandon),
        ("emitIndependent", emit_independent),
        ("runTask", run_task),
    ]


def violations(st, variant):
    """返回该状态违反的不变式列表。"""
    alive, strong, weak, roots = st
    out = []
    # NoLeak：活对象必须从 ROOT 强可达（GC 语义下不可达当活 ⇔ 强边成环）
    reach = {ROOT}
    frontier = True
    while frontier:
        frontier = False
        for (a, b) in strong:
            if a in reach and b not in reach and b in alive:
                reach.add(b)
                frontier = True
    leaked = [o for o in alive if o not in reach]
    if leaked:
        out.append("NoLeak:" + ",".join(sorted(leaked)))
    # AsyncNoUseAfterDead：异步任务在写其条目源时，源背后的读者已被销毁
    # （weakOnly 变体的预期违例——"两者必须配套"的形式化证据）
    if variant != "legacy" and any(a == "T" and b == "E" for (a, b) in strong):
        if "R" not in alive:
            out.append("AsyncNoUseAfterDead")
    return out


def explore(variant):
    """BFS 全序列穷举（≤MAX_DEPTH 步）。返回 (最短反例序列 | None, 违例不变式集合)。"""
    init = (frozenset(), frozenset(), frozenset(), frozenset())
    seen = {canon(*init)}
    frontier = [(init, ())]
    worst = set()
    example = None
    while frontier:
        nxt = []
        for st, path in frontier:
            bad = violations(st, variant)
            if bad:
                worst.update(bad)
                if example is None:
                    example = path
                continue   # 反例状态不再展开（报告最短序列即可）
            if len(path) >= MAX_DEPTH:
                continue
            for name, fn in ops(variant):
                st2 = fn(st)
                if st2 is None:
                    continue
                c = canon(*st2)
                if c in seen:
                    continue
                seen.add(c)
                nxt.append((st2, path + (name,)))
        frontier = nxt
    return example, worst


def main():
    ok = True
    expect = {
        "legacy": {"NoLeak"},                 # 修复前：必须复现泄漏（校准）
        "weakOnly": {"AsyncNoUseAfterDead"},  # 无 KeepAlive：异步用后死亡
        "fixed": set(),                       # weak + KeepAlive：全序列无违例
    }
    for variant in ("legacy", "weakOnly", "fixed"):
        example, worst = explore(variant)
        got = {v.split(":")[0] for v in worst}
        status = "OK" if got == expect[variant] else "UNEXPECTED"
        if got != expect[variant]:
            ok = False
        print(f"[model] {variant:9s} 深度≤{MAX_DEPTH} 全序列: 违例={sorted(worst) or '无'}"
              f" 期望={sorted(expect[variant]) or '无'} → {status}")
        if example:
            print(f"         最短反例: {' → '.join(example)}")
    if not ok:
        print("[model] 验收门失败：模型行为与预期不符")
        return 1
    print("[model] 验收门通过：修复前复现 replayQ_ 泄漏反例；"
          "仅 weak 无 KeepAlive 出现异步用后死亡（配套性证明）；weak+KeepAlive 全序列无违例")
    return 0


if __name__ == "__main__":
    sys.exit(main())
