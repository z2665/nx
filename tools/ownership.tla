---- MODULE ownership ----
EXTENDS Naturals, FiniteSets
(* nx 所有权模型（roadmap §7.4 / §8 批次 4 前置验收门，TLA+/TLC 权威形态）--------
 *
 * 对象：R=ContainerReader，E=EntrySource，S=SpoolBuffer，V=View，T=SinkTask，
 *       K=KeepAlive 令牌（Root 为根伪节点）。
 * 变体（CONSTANT Variant）：
 *   "legacy"    修复前：probe 把持读者强回指的条目源存进读者自己的重放队列
 *               （replayQ_ 自引用环语义）
 *   "weakOnly"  仅 weak_ptr、无 KeepAlive（"两者必须配套"的反面）
 *   "fixed"     weak_ptr + Sink 任务持 KeepAlive 令牌
 * 不变式：
 *   NoLeak             活而不可达的对象必须可被追踪 GC 回收（即非环）——
 *                      修复前语义必违（校准），fixed 必满足
 *   AsyncNoUseAfterDead 异步写出任务存活期间其条目源背后的读者必须存活——
 *                      weakOnly 必违（配套性证明），fixed 必满足
 * GC 建模：Collect 为独立原子动作（迭代零入度定点），比"操作内折叠"更忠实。
 *--------------------------------------------------------------------------
 *)
CONSTANTS R, E, S, V, T, K, Root
CONSTANT Variant

Objects == {R, E, S, V, T, K}
Nodes == Objects \union {Root}

VARIABLES alive,   (* 活对象集（Root 恒活，不入此集） *)
          strong,  (* 强所有权边（含 Root 出边 = 根集） *)
          weak     (* 弱引用边（不参与存活/回收判定） *)

vars == <<alive, strong, weak>>

TypeOK == /\ alive \subseteq Objects
          /\ strong \subseteq (Nodes \X Nodes)
          /\ weak \subseteq (Objects \X Objects)

(* 迭代零入度定点：i 轮内可回收的对象（追踪 GC） *)
RECURSIVE Collectable(_)
Collectable(i) ==
  IF i = 0 THEN {}
  ELSE LET prev == Collectable(i - 1)
       IN prev \union {o \in alive \prev :
             ~\E e \in strong : /\ e[2] = o
                                /\ (e[1] = Root \/ e[1] \in alive)
                                /\ e[1] \notin prev}
Dead == Collectable(Cardinality(Objects) + 1)

(* 强可达集：从 Root 沿强边 *)
RECURSIVE ReachIn(_)
ReachIn(i) ==
  IF i = 0 THEN {Root}
  ELSE LET prev == ReachIn(i - 1)
       IN prev \union {t \in alive : \E e \in strong : /\ e[1] \in prev
                                               /\ e[2] = t}
Reach == ReachIn(Cardinality(Objects) + 1)

(* 不变式 ------------------------------------------------------------------ *)
NoLeak == (alive \ Reach) \ Dead = {}
AsyncNoUseAfterDead ==
  ~/\ T \in alive
    /\ <<T, E>> \in strong      (* 任务持条目源 *)
    /\ R \notin alive         (* 而其读者已销毁 *)

Init == /\ alive = {}
        /\ strong = {}
        /\ weak = {}

(* 打开容器：读者 + spool + 视图诞生，根持读者 *)
Open == /\ R \notin alive
        /\ alive' = alive \union {R, S, V}
        /\ strong' = strong \union {<<Root, R>>, <<R, S>>, <<R, V>>, <<V, S>>}
        /\ weak' = weak

(* probe 预取：legacy 把持强回指的条目源塞进读者自己的重放队列（环成因）；
 * fixed 重放队列只存元数据（ReplayRecord），此处无堆事件 *)
ProbePush == /\ R \in alive
             /\ Variant = "legacy"
             /\ alive' = alive \union {E}
             /\ strong' = strong \union {<<R, E>>, <<E, R>>}
             /\ weak' = weak

(* next() 新条目：条目源由调用方（walk 栈，经 Root 表意）持有；
 * 对读者 legacy 持强边、其余变体持弱边 *)
NextEntry == /\ R \in alive
             /\ E \notin alive
             /\ alive' = alive \union {E}
             /\ weak' = IF Variant = "legacy" THEN weak
                        ELSE weak \union {<<E, R>>}
             /\ strong' = IF Variant = "legacy"
                          THEN strong \union {<<Root, E>>, <<E, R>>}
                          ELSE strong \union {<<Root, E>>}

(* 打开失败（触发族 1）/ 中途弃置（触发族 2）：根释放读者 *)
DropReader == /\ R \in alive
              /\ strong' = strong \ {<<Root, R>>}
              /\ alive' = alive
              /\ weak' = weak

(* 独立源 → Sink 异步写：任务持条目源；fixed 另持 KeepAlive（别名构造持读者） *)
EmitIndependent == /\ E \in alive
                   /\ T \notin alive
                   /\ R \in alive
                   /\ alive' = IF Variant = "fixed"
                               THEN alive \union {T, K}
                               ELSE alive \union {T}
                   /\ strong' = LET base == strong \union {<<Root, T>>, <<T, E>>}
                                IN IF Variant = "fixed"
                                   THEN base \union {<<T, K>>, <<K, R>>}
                                   ELSE base
                   /\ weak' = weak

(* 任务完成 *)
RunTask == /\ T \in alive
           /\ strong' = strong \ {<<Root, T>>}
           /\ alive' = alive
           /\ weak' = weak

(* 追踪 GC：回收零入度定点 *)
Collect == /\ Dead # {}
           /\ alive' = alive \ Dead
           /\ strong' = {e \in strong : /\ e[1] \notin Dead
                                      /\ e[2] \notin Dead}
           /\ weak' = {e \in weak : /\ e[1] \notin Dead
                                   /\ e[2] \notin Dead}

Next == \/ Open
        \/ ProbePush
        \/ NextEntry
        \/ DropReader
        \/ EmitIndependent
        \/ RunTask
        \/ Collect

Spec == Init /\ [][Next]_vars

====
