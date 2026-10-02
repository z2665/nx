// Smoke.fst：F* 链路自检（z3 自动化 + 迭代闭包性质——批次 5 证明侧雏形）
module Smoke

open FStar.List.Tot
open FStar.List.Tot.Properties

type node = int

(* 图上一轮扩展：frontier ∪ 所有后继；种子元素保留 *)
val step : edges:(node -> list node) -> frontier:list node
  -> Pure (list node)
           (requires True)
           (ensures fun r -> (forall (x: node). List.mem x frontier ==> List.mem x r))
let step edges frontier =
  let rest = flatten (map edges frontier) in
  let r = append frontier rest in
  append_memP_forall frontier rest;   (* mem(memP 桥接自动) 在 append 两侧的成员式 *)
  r

(* 迭代闭包：种子元素永不丢失（归纳于 n） *)
val closure : edges:(node -> list node) -> seed:list node -> n:nat
  -> Pure (list node)
           (requires True)
           (decreases n)
           (ensures fun r -> (forall (x: node). List.mem x seed ==> List.mem x r))
let rec closure edges seed n =
  if n = 0 then seed
  else step edges (closure edges seed (n - 1))

(* 单调性：第 n+1 轮闭包包含第 n 轮 *)
val closure_monotone : edges:(node -> list node) -> seed:list node -> n:nat
  -> Lemma (ensures (forall (x: node).
      List.mem x (closure edges seed n) ==> List.mem x (closure edges seed (n + 1))))
let closure_monotone edges seed n =
  let r1 = closure edges seed n in
  let r2 = closure edges seed (n + 1) in
  assert (r2 == append r1 (flatten (map edges r1)));
  append_memP_forall r1 (flatten (map edges r1));
  ()
