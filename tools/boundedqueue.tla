---- MODULE boundedqueue ----
EXTENDS Naturals, FiniteSets

(* nx BoundedQueue abandon 协议模型（roadmap §7.4 遗留项，2026-10-03）-----------
 *
 * 对象：src/pipes.hpp 的 BoundedQueue——MPSC 有界队列，满则 push 阻塞（背压），
 *       close()=生产者收尾（唤醒消费侧），abandon()=消费者消失（QueueSource
 *       析构，walk 栈正常/异常展开皆然）：置 dead + 清队 + 双侧 notify_all。
 *
 * 建模约定（忠实性）：
 *   - 条目计数抽象：被验性质（阻塞/唤醒/终态）不依赖 FIFO 序与条目身份；
 *   - 单生产者：nx 实际部署形态（filter_decode 泵 → 唯一生产者 → 收尾 close）。
 *     多生产者 notify_one 配对属公平性/活性范畴，TLC 安全检查不覆盖，不建模；
 *   - 条件变量唤醒 = parked→ready 原子迁移（cv 契约：被通知者必将复查）；
 *     另设 SpuriousWake 任意迁移——while 循环复查纪律须对伪唤醒安全；
 *   - dead 为终态（无动作复位），与实现一致。
 *
 * 变体（CONSTANT Variant，校准反例）：
 *   "fixed"           实现现状
 *   "closeNoWake"     close 只置位不唤醒消费侧 → 消费者永驻（ParkedSanity 必违）
 *   "abandonNoWake"   abandon 只置位不唤醒 → dead 仍有驻留者（DeadRelease 必违）
 *
 * 不变式：
 *   DeadRelease      dead ⇒ 双侧无驻留（"dead 时两侧必不阻塞"，roadmap §5.1
 *                    BoundedQueue 状态机行；F4 死锁窗口的协议级保证）
 *   ParkedSanity     驻留前提纪律=while 条件：生产者驻留 ⇒ 满且有货未推且未死；
 *                    消费者驻留 ⇒ 空且未关且未死（NoLostWake 的安全形态：
 *                    唤醒不丢——有数据必有人可动）
 *--------------------------------------------------------------------------
 *)
CONSTANTS Cap, NPUSH
CONSTANT Variant

VARIABLES queued,   (* 队内条目数 *)
          pstate,   (* 生产者：ready | parked | finished *)
          rm,       (* 生产者剩余推送数 *)
          cstate,   (* 消费者：ready | parked | retired *)
          closed, dead

vars == <<queued, pstate, rm, cstate, closed, dead>>

TypeOK == /\ queued \in 0..Cap
          /\ pstate \in {"ready", "parked", "finished"}
          /\ rm \in 0..NPUSH
          /\ cstate \in {"ready", "parked", "retired"}
          /\ (pstate \in {"ready", "parked"} => rm > 0)

(* ---- 不变式 -------------------------------------------------------------- *)
DeadRelease == dead => /\ pstate # "parked"
                       /\ cstate # "parked"

ParkedSanity ==
  /\ (pstate = "parked" => /\ queued = Cap
                           /\ rm > 0
                           /\ ~dead)
  /\ (cstate = "parked" => /\ queued = 0
                           /\ ~closed
                           /\ ~dead)

(* ---- 初态 ---------------------------------------------------------------- *)
Init == /\ queued = 0
        /\ pstate = "ready"
        /\ rm = NPUSH
        /\ cstate = "ready"
        /\ closed = FALSE
        /\ dead = FALSE

(* ---- 动作 ---------------------------------------------------------------- *)
(* push：dead ⇒ false（值丢弃，生产者退场）；满 ⇒ 驻留；否则入队并唤醒消费侧 *)
Push ==
  /\ pstate = "ready"
  /\ rm > 0
  /\ IF dead
       THEN /\ pstate' = "finished"
            /\ rm' = 0
            /\ UNCHANGED <<queued, cstate, closed, dead>>
       ELSE IF queued < Cap
              THEN /\ queued' = queued + 1
                   /\ rm' = rm - 1
                   /\ pstate' = IF rm = 1 THEN "finished" ELSE "ready"
                   /\ (\/ /\ cstate = "parked"    (* emptyCv_.notify_one *)
                         /\ cstate' = "ready"
                      \/ /\ cstate # "parked"
                         /\ cstate' = cstate)
                   /\ UNCHANGED <<closed, dead>>
              ELSE /\ pstate' = "parked"          (* fullCv_.wait *)
                   /\ UNCHANGED <<queued, rm, cstate, closed, dead>>

(* pop：dead ⇒ nullopt 退场；有关出队并唤醒生产侧；空且 closed ⇒ EOF 退场；
 *       空且 open ⇒ 驻留 *)
Pop ==
  /\ cstate = "ready"
  /\ IF dead
       THEN /\ cstate' = "retired"
            /\ UNCHANGED <<queued, pstate, rm, closed, dead>>
       ELSE IF queued > 0
              THEN /\ queued' = queued - 1
                   /\ cstate' = "ready"
                   /\ (\/ /\ pstate = "parked"    (* fullCv_.notify_one *)
                         /\ pstate' = "ready"
                      \/ /\ pstate # "parked"
                         /\ pstate' = pstate)
                   /\ UNCHANGED <<rm, closed, dead>>
              ELSE IF closed
                     THEN /\ cstate' = "retired"   (* closed 且排空 → EOF *)
                          /\ UNCHANGED <<queued, pstate, rm, closed, dead>>
                     ELSE /\ cstate' = "parked"    (* emptyCv_.wait *)
                          /\ UNCHANGED <<queued, pstate, rm, closed, dead>>

(* close：生产者收尾（唯一泵线程结束）——emptyCv_.notify_all；
 * closeNoWake 校准变体只置位不唤醒 *)
Close ==
  /\ pstate = "finished"
  /\ ~closed
  /\ ~dead
  /\ closed' = TRUE
  /\ cstate' = IF cstate = "parked" /\ Variant # "closeNoWake"
               THEN "ready"
               ELSE cstate
  /\ UNCHANGED <<queued, pstate, rm, dead>>

(* abandon：消费者消失（QueueSource 析构）——dead + 清队 + 双侧 notify_all；
 * abandonNoWake 校准变体只置位不唤醒 *)
Abandon ==
  /\ ~dead
  /\ dead' = TRUE
  /\ queued' = 0
  /\ pstate' = IF pstate = "parked" /\ Variant # "abandonNoWake"
               THEN "ready"
               ELSE pstate
  /\ cstate' = IF cstate = "parked" /\ Variant # "abandonNoWake"
               THEN "ready"
               ELSE cstate
  /\ UNCHANGED <<closed, rm>>

(* 伪唤醒：cv 允许无通知返回——while 复查纪律须对其安全（多出的状态更严） *)
SpuriousWakeP ==
  /\ pstate = "parked"
  /\ pstate' = "ready"
  /\ UNCHANGED <<queued, rm, cstate, closed, dead>>

SpuriousWakeC ==
  /\ cstate = "parked"
  /\ cstate' = "ready"
  /\ UNCHANGED <<queued, pstate, rm, closed, dead>>

(* 正常终态自环：全退场（避免把收尾完成误记为 TLC 死锁） *)
Terminal ==
  /\ pstate = "finished"
  /\ cstate = "retired"
  /\ dead \/ closed
  /\ UNCHANGED <<queued, pstate, rm, cstate, closed, dead>>

Next == \/ Push
        \/ Pop
        \/ Close
        \/ Abandon
        \/ SpuriousWakeP
        \/ SpuriousWakeC
        \/ Terminal

Spec == Init /\ [][Next]_vars

====
