// Closure.fst：AST 强闭包检查器的内核规范与正确性（批次 5）
// 数学核心：给定类型间强所有权边表 edges，判定 esft 类的成员强闭包是否含自身
// （= 类型级自引用环，replayQ_ 事故的静态形状）。
// 证明责任分层：P0 集合操作保元素 / P1 迭代保种子 / P2 逐轮单调 / P3 后继吸收
// ——四者合成"迭代闭包 ⊇ 可达集"的不动点论证骨架。
// self_cycle 为抽取目标（krml→C）。信任边界：本证明覆盖"给定边表的判定正确"，
// 边表抽取完备性由校准标准（f647037 恰报 LaSeqReader 零误报）保障。
module Closure

open FStar.List.Tot
open FStar.List.Tot.Properties

type node = UInt32.t

(* 去重插入 / 并入（结构递归保终止；只增不减） *)
let insert (x: node) (l: list node) : Tot (list node) =
  if List.mem x l then l else x :: l

let rec union_acc (l: list node) (acc: list node)
  : Tot (list node) (decreases l)
  = match l with
    | [] -> acc
    | x :: q -> union_acc q (insert x acc)

let step (edges: node -> list node) (frontier: list node) : Tot (list node) =
  union_acc (flatten (map edges frontier)) frontier

(* P0a：insert 不丢 acc 元素 *)
val insert_keeps : x:node -> l:list node
  -> Lemma (ensures (forall (y: node). List.mem y l ==> List.mem y (insert x l)))
let insert_keeps _ _ = ()

(* P0b：union_acc 两侧元素都保留 *)
val union_keeps : l:list node -> acc:list node
  -> Lemma (ensures (forall (y: node).
      (List.mem y l || List.mem y acc) ==> List.mem y (union_acc l acc)))
let union_keeps l acc =
  let rec aux (k: list node) (a: list node)
    : Lemma (ensures (forall (y: node).
              (List.mem y k || List.mem y a) ==> List.mem y (union_acc k a)))
            (decreases k)
    = match k with
      | [] -> ()
      | x :: q -> insert_keeps x a; aux q (insert x a)
  in aux l acc

(* 迭代闭包：closure 0 = {x}；每轮吸收后继 *)
let rec closure (edges: node -> list node) (fuel: nat) (x: node)
  : Pure (list node) (decreases fuel)
  = if fuel = 0 then [x]
    else step edges (closure edges (fuel - 1) x)

(* P1：种子保留 *)
val closure_keeps_seed : edges:(node -> list node) -> x:node -> n:nat
  -> Lemma (ensures List.mem x (closure edges n x))
let closure_keeps_seed edges x n =
  let rec aux (k: nat)
    : Lemma (ensures List.mem x (closure edges k x)) (decreases k)
    = if k = 0 then ()
      else (
        let ck = closure edges (k - 1) x in
        union_keeps (flatten (map edges ck)) ck;
        aux (k - 1)
      )
  in aux n

(* P2：逐轮单调 *)
val closure_mono : edges:(node -> list node) -> x:node -> n:nat
  -> Lemma (ensures (forall (y: node).
      List.mem y (closure edges n x) ==> List.mem y (closure edges (n + 1) x)))
let closure_mono edges x n =
  let rec aux (k: nat)
    : Lemma (ensures (forall (y: node).
              List.mem y (closure edges k x) ==> List.mem y (closure edges (k + 1) x)))
            (decreases k)
    = if k = 0 then (
        union_keeps (flatten (map edges [x])) [x]
      ) else (
        assert (k - 1 + 1 == k /\ k + 1 - 1 == k);
        let ck = closure edges (k - 1) x in
        let ck1 = closure edges k x in
        let c2 = closure edges (k + 1) x in
        assert (ck1 == step edges ck);
        assert (c2 == step edges ck1);
        union_keeps (flatten (map edges ck)) ck;       (* 旧闭包 ⊆ 新闭包 *)
        union_keeps (flatten (map edges ck1)) ck1;     (* 新闭包 ⊆ 再下一轮 *)
        aux (k - 1)
      )
  in aux n

(* P3 前置：成员的后继落在 flatten(map edges) 中 *)
val succ_in_flatten : edges:(node -> list node) -> l:list node -> y:node -> z:node
  -> Lemma (requires (List.mem y l /\ List.mem z (edges y)))
            (ensures List.mem z (flatten (map edges l)))
let succ_in_flatten edges l y z =
  let rec aux (k: list node)
    : Lemma (requires List.mem y k)
             (ensures List.mem z (flatten (map edges k)))
             (decreases k)
    = match k with
      | [] -> ()
      | h :: q ->
          assert (flatten (map edges (h :: q)) == append (edges h) (flatten (map edges q)));
          if h == y then append_memP_forall (edges h) (flatten (map edges q))
          else (
            append_memP_forall (edges h) (flatten (map edges q));
            aux q
          )
  in aux l

(* P3：后继吸收——闭包成员的后继在下一轮进入闭包（点式） *)
val closure_absorbs : edges:(node -> list node) -> x:node -> n:nat -> y:node -> z:node
  -> Lemma (ensures (List.mem y (closure edges n x) ==> List.mem z (edges y)
                    ==> List.mem z (closure edges (n + 1) x)))
let closure_absorbs edges x n y z =
  let c1 = closure edges n x in
  let c2 = closure edges (n + 1) x in
  if List.mem y c1 && List.mem z (edges y) then (
    assert (c2 == step edges c1);
    union_keeps (flatten (map edges c1)) c1;
    succ_in_flatten edges c1 y z
  ) else ()

(* 检查器判定：fuel = 全域大小+1 时闭包是否含自身 *)
val self_cycle : edges:(node -> list node) -> universe_size:nat -> x:node
  -> Pure bool
           (requires True)
           (ensures fun r -> r <==> List.mem x (closure edges (universe_size + 1) x))
let self_cycle edges universe_size x =
  List.mem x (closure edges (universe_size + 1) x)
