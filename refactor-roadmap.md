# nx 现代化重构路线图（refactor-roadmap）

> **版本**：v2.1（2026-10-02 批次 0 完成回填）　**基线**：commit `f647037`（行号引用）
> **配套**：[README.md](README.md)（进度权威）· [AGENTS.md](AGENTS.md)（工作区纪律）·
> [nested-extractor-design.md](nested-extractor-design.md)（设计权威）
>
> **阅读指南**：§1–3 看动机与原则；§4 缺陷登记簿（**D1-D8 已全部修复**，含验收记录）；
> §5 目标架构；§6 四路审计详录（材料库，按需查阅）；§7 形式化验证方案（S1-S5 已落地）；
> §8 执行计划（单一批次制，批次 0 已完成）；§9 决策记录。全部行号为 f647037 基线。

---

## 1 背景与动机

### 1.1 真实案例驱动的调查

三个 15GB 级真实语料案例先后暴露了管线在**正确性、健壮性、性能**三个维度的债务，
调查过程沉淀为本路线图的事实基础：

| 案例 | 症状 | 根因 | 当前状态 |
|---|---|---|---|
| 案例 XJ（2.6GB） | 解压外推 8~12 小时 | solid 7z 逐条目单独 `Extract` = 每文件从 solid 块头重解码（O(N²)） | **已修复**（批式抽取，38s/3747 文件，2f5494b） |
| 案例 L（15GB） | 最深路径文件落盘失败 | 输出路径 251 字符 + `.nxpart-` 后缀 > 260，Sink 三处裸路径调用无 `\\?\` | **已修复**（f647037） |
| 同上 | 14.86GB `nx-{GUID}.tmp` 成功运行后残留 | **LaSeqReader replayQ_ 自引用环**（见 §1.2） | 文件残留已由 DELETE_ON_CLOSE 兜底；**对象泄漏未修**（D6） |

### 1.2 泄漏根因定案：replayQ_ 自引用环

`LaSeqReader`（engines.cpp，`enable_shared_from_this`）的 `probeFirst()` 预取条目时，
每个 `ContainerEntry.data = LaEntrySource(shared_from_this())` 持有指回读取器自身的
`shared_ptr`，存进读取器**自己的** `replayQ_` 成员：

```
LaSeqReader ─own→ replayQ_ ─own→ ContainerEntry.data ─own→ LaEntrySource ─own→ LaSeqReader
```

环成立 → 打开失败被丢弃的读取器永不析构 → 连带 `spool_` 与视图（退出时稳定的
2 个幽灵强引用；DELETE_ON_CLOSE 之前即 14.8GB 临时文件残留）。
仪器化铁证：失败尝试的读取器与其视图**零析构日志**，成功尝试正常析构。

**两个触发族**（回归需都覆盖）：
1. 打开失败：加密嵌套 zip 的无密码/错密码首轮尝试；
2. **成功打开但中途弃置**：replayQ_ 未排空时因分支错误/取消/keepGoing=false 上抛。

**教训（升格为全局纪律）**：esft 类型把 `shared_from_this()` 交给"将被自己持有的
结构"＝制造环。领域审计进一步论证：这不是孤立 bug，而是 `ContainerReader` 契约缺
"迭代位置类型"的必然产物（§5.3 EntryToken）。

### 1.3 四路独立审计概览（2026-10-02，基线 f647037）

| 审计视角 | 核心结论 |
|---|---|
| 现代 C++ | 水准较高（span/enum class/jthread/SecureStr）；4 个真实缺陷 + 手工资源散布 + C 风格日志体系 |
| 领域建模 | 策略层知识全部以"自由函数+裸字符串+平行重复实现"存在；14 项抽象提议 |
| 函数式化 | 两个全局隐式依赖（g_prog/g_nameCp）+ 竞态 g_quiet；纯化集中在决策层；热路径有明确红线 |
| 所有权/形式化 | 穷举 95 节点 50 边：**唯一真实环 = replayQ_**；四条无断言守护的借用纪律；AST 闭包检查器可永久免疫 |

**交叉印证**（多审计独立发现，置信度高）：`firstHardError` 竞态（3 路）、
`g_nameCp`/`g_prog` 隐式全局（2 路）、两套 seek 视图逐行重复（2 路）、
`ascii_lower` 循环 7 处重复（2 路）。

---

## 2 目标与非目标

### 2.1 目标

1. 所有权关系构成可证明无环的 DAG，泄漏可被哨兵/静态检查自动捕获；
2. 一切手动释放资源圈禁到单一模块，其余代码只见 RAII 类型；
3. 领域概念显式类型化（格式/层路径/消毒/码表/分片组/密码链），消灭平行重复实现；
4. 纯核心/效果壳分离：决策逻辑确定性可单测，副作用集中显式;
5. 所有权与关键协议有形式化模型与验证手段（§7）。

### 2.2 非目标 / 硬边界（每批验收的组成部分）

- **行为语义零变化**：免 spool 直读及其回退语义、密码解析链顺序、深度/总量/压缩比/
  磁盘水位熔断、退出码契约（0/1/2/3/4/64）、`.part` 原子落名——除非经 §9 决策显式变更；
- **性能红线**：filter 五解码泵、readEntryDirect 块交付状态机、PushbackSource、
  writeOne 写出循环、SFX/EOCD 扫描体**不做纯度牺牲**（ranges 化红线，理由见 §6.3）；
- 密码纪律：绝不入日志/报告，SecureStr 边界只收紧不放松；
- 热路径零拷贝（D5 直通视图）不可引入间接层。

---

## 3 设计原则

| # | 原则 | 含义 |
|---|---|---|
| P1 | **所有权单向流动** | Walker → ContainerReader → ByteSource 树；唯一合法"父方向"强边是条目源→读取器（顺序容器语义必需），前提是读取器成员永不回存条目源 |
| P2 | **泄漏调用圈禁** | Win32 句柄 / COM 引用 / 临时文件只允许出现在 `src/res/`，其余代码经 RAII 类型使用 |
| P3 | **泄漏可观测** | 常驻哨兵 + 静态审计脚本 + fuzz 断言，回归即抓 |
| P4 | **值语义与 RAII 优先** | 消灭裸 new/裸句柄/手工清理；输出参数换返回值；错误按值表达 |
| P5 | **纯核心 / 效果壳** | 解析、推导、打分、消毒、决策为纯函数；IO/日志/GUI/线程集中为显式实例的壳 |
| P6 | **不变式显式化并可验证** | 所有权 DAG、生命周期状态机、借用先于亡——写成断言/检查器/模型，不留在注释里 |

---

## 4 缺陷登记簿（P0 —— 全部先行，合计约 2.5 天）

> 以下为已确认的真实缺陷，与重构解耦、可独立发布。每条含验收标准。

| 编号 | 问题 | 位置 | 修复方案 | 量 | 状态 |
|---|---|---|---|---|---|
| D1 | **命令行密码明文写入 nx.log**（运行头记录完整命令行） | log.cpp:71-81 | 写前对 argv 红线过滤：`-p/--password/--password-file` 的值替换 `***` | 15 行 | **已修复**（36d04d1） |
| D2 | `--depth abc` 等非法参数 → `std::terminate`（stoi 标准异常未被捕获，违反退出码契约应 64） | main.cpp:230-232 | `std::from_chars`，失败抛 nx::Error | 10 行 | **已修复**（029b8da） |
| D3 | `Stats::firstHardError`（std::string）跨线程无锁读写（写持锁 sink.cpp:122-128；读无锁 sink.cpp:176、walker.cpp:149） | session.hpp:40 | HardErrorSlot：锁内快照；或 atomic 标志+锁内取消息 | 15 行 | **已修复**（1e90f08，HardErrorSlot 自带互斥） |
| D4 | `win_long_path` 路径超 1040 字符时采纳**未初始化栈缓冲**（GetFullPathNameW 超长不写缓冲） | util.cpp:39-49 | 两次调用协议（先取所需长度再按需分配） | 20 行 | **已修复**（0f8e937，1248 字符实测） |
| D5 | `g_quiet` 普通 bool 跨线程读写（fuzz/GUI/工作线程并发） | log.cpp:18 | `std::atomic<bool>` | 1 行 | **已修复**（33b4c8c） |
| D6 | **replayQ_ 自引用环**（§1.2，两个触发族） | engines.cpp:460 等 | replayQ_ 只存 `{idx,name,meta}`，重放时重建 LaEntrySource——环结构性不可再形成；dtor 断言 `replayQ_.empty()` | 半天 | **已修复**（44503e3，ReplayRecord 重放重建；校准见下 S3） |
| D7 | **过滤器链递归深度无上界**（容器分支才查 maxDepth，过滤器分支同 depth 递归）——4MiB 输入可构造数千层嵌套 gzip → 栈溢出/线程耗尽（DoS） | walker.cpp:341-347 | **已决策（决策 D-1，§9）**：过滤器链纳入 `--depth` 约束（每容器段内链长 ≤ maxDepth，容器段重置），默认 8→10 | 半天 | **已修复**（12ec147） |
| D8 | 小项合集：F3 死代码（LaSeqReader `reader` 形参不存成员即悬空、`SpoolBuffer::reader()` 全仓无调用）；F4 jthread/qs 构造顺序 OOM 死锁窗口；L4 `.Z` 后缀永不匹配（已小写化）；M7 `Sink::note` 直写 printf 绕过 log/quiet | engines.cpp:343、spool.hpp:40、walker.cpp:341、format.hpp:60-73、sink.cpp:118 | 删除/顺序调整/统一小写/走 `log_out` | ~20 行 | **已修复**（09d8850） |

**D 批验收门**：✅ 49 属性 + 9 GUI 全绿（46→49：filter_depth_bomb / pw_retry_nested /
arg_validation）；新增回归全部落地：加密嵌套 zip 失败重试 + 中途弃置（pw_retry_nested
两形态，D6 触发族 1）、过滤器链深度（filter_depth_bomb，30 层 gzip 期望退出码 3）、
`--depth abc` 期望 64（arg_validation）、nx.log 无密码残留（logging 内 grep 断言）。
触发族 2（中途弃置）由哨兵在 fuzz 下覆盖（maxBytes 熔断/取消路径高频触发弃置）。
**哨兵 S1-S5 已随行落地**（8dfe638，diag 模块 + fuzz 常开 + NX_DIAG_LEAKS_MAIN 诊断构建），
校准实验：带环旧实现 + 前置目录加密 zip 无密码 → S3 案发现场 abort（进 0 出 1）；
修复后同输入 exit 2 静默。D6 的 dtor 断言（S6）未做：重放队列类型层面已不可能持条目源，
编译期保证强于运行时断言（弃置场景队列合法非空，断言反会误报）。

---

## 5 目标架构

### 5.1 所有权模型

**图结构**（详录 §6.4）：强所有权边在修复 replayQ_ 环后，构成以
{main 栈 Session、Walker 活动栈帧、Sink 任务队列、7z.dll COM 引用} 为根的 **DAG**。

**核心机制**：
- **weak_ptr + KeepAlive 令牌**：条目源（LaEntrySource/EntrySource）对读取器改持
  `weak_ptr`；异步写出的存活由 Sink 任务持有 `shared_ptr<void>` 保活令牌
  （读取器 `keepAlive()` 别名构造提供）——两者必须配套落地；
- **生命周期状态机**（每类一页进设计文档 §4）：

| 类型 | 状态机 | 关键不变式 |
|---|---|---|
| SpoolBuffer | growing → finished（overflowed 单调） | read_at 仅 finished；append 仅 growing；overflowed ⇒ ram_.empty() |
| LaSeqReader | probing → iterating → drained → dead（可 failed） | **INV-REPLAY：dead 时 replayQ_ 必空**；adoptStream 至多一次 |
| SevenZipReader | opening⇄enumerate⇄probe → ready → iterating → dead | arc_≠null ⇒ ready/iterating；cache_[i].pos 单调 |
| Sink/ThreadPool | accepting → draining → destroyed | **INV-SINK：destroyed 仅可自 draining 迁入**（先 waitAll） |
| BoundedQueue | open/closed → dead（终态） | dead 时两侧必不阻塞 |

- **借用纪律断言化**（四条）：Sink 析构前必 waitAll（~ThreadPool/~Sink idle 断言）；
  QueueSource 先亡于队列（F4 顺序修正）；Session/Sink 成员声明序（注释显式化）；
  COM 释放契约（余额哨兵 S8）。

### 5.2 模块划分

```
src/
  res/        ← P2 圈禁：UniqueFile（RAII 句柄）、TempFile（唯一临时文件工厂，
               │            统一 DELETE_ON_CLOSE）、com_ptr（COM 收编）
               │  释放调用（CloseHandle/DeleteFileW/Release）从此只存在于此
  纯核心      ← P5：detect_from_bytes / parse_atom_header / eocd_from_window /
               │  score_w·charset_candidates / sanitize·NamePolicy / ascii_lower /
               │  match_split_name·group_volumes·select_group / render_report /
               │  classify_branch / derive_exit_code
  效果壳      ← IO 对象（FileSource/FileSeekView/SpoolBuffer/RegionView——合并后唯一实现）
               进程单例（Logger / SzDll）；会话实例（ProgressUi / PasswordProvider）
               编排壳（walker / sink / main）
  engines 拆分：namecodec / views（两套 seek 视图合并）/ laseq / zipcd / open（策略）
```

**视图合并**是圈禁的前置：engines.cpp 与 szcom.cpp 各自维护一套
SeekView/SeekInput（文件/spool/region 三态，近逐行重复，含 meter 挂载点）→
统一 `make_file_view / make_spool_view / make_region_view` 三工厂 +
`MeteredViewFactory` 把"挂表纪律"（根输入挂/派生流不挂）从注释变类型。
合并后 CreateFileW 散布点 5→3，直接给 res/ 圈禁减负。

### 5.3 领域类型清单

| 概念 | 现状 | 目标类型 | 审计来源 |
|---|---|---|---|
| 格式知识 | 三处散落（format.hpp 双 switch + detect 魔数链 + filter 第三次写） | `constexpr kFormatTable` + `consteval format_info()` | 领域 #1 |
| 层身份 | layerId 拼接 5 处 3 格式；**深度+basename 作密码缓存键 → 兄弟分支共享缓存（语义漂移）** | `LayerPath`（logical 路径/display/cacheKey）+ `LayerCtx`（descend 唯一派生，收敛 walk 8 参数） | 领域 #2 |
| 迭代契约 | 预取重放内藏（replayQ_ 环的结构根源）；条目流失效靠运行时判 idx | `EntryToken`（迭代序号+independent）+ `read(token)` 显式索取 | 领域 #10 |
| 数据相位记录 | CbCtx public 字段被回调写穿 | `ViewPort`（variant）+ `AccessRecorder`（私有状态机） | 领域 #3 / 函数式 2.1 |
| 文件名码表 | g_nameCp 进程粘性（多输入串包） | `NameCodec` 每读取器一份 | 领域 #5 |
| 路径消毒 | 三合一藏于 Sink::dedupe（持锁），不可单测 | `NamePolicy`（纯）+ `UsedNames` + `SafePath` 值类型 | 领域 #8 |
| 分片分组 | 两套平行实现（FS 级 group_volumes vs 条目级 90 行状态机）+ 4 处开卷编排 | `VolumeGroupBuilder`（增量）+ `open_volume_set` 单口 | 领域 #6 |
| 密码链 | 隐式平坦索引；prompt 硬编码不可注入 | `PromptSink` 注入 + 显式尝试流——可脚本化单测 | 领域 #7 |
| 退出码 | main if 链，无类型无测试 | `enum class Outcome` + `derive_exit_code()` | 领域 #4 |
| Session | god context（定义在 walker.hpp） | `Walker` 类 + `resolve_options` 装配单点 | 领域 #11 |
| 检测注记 | SFX 偏移序列化成字符串丢弃 | `Detection.note` variant + display() | 领域 #12 |
| 进度 UI | g_prog 裸全局 | `ProgressUi` 挂 Session，Win32 回调经 USERDATA 传实例 | 函数式 1.1 |
| 日志 | 四全局 + printf 门面 | `Logger` 单例（保留自由函数门面） | 函数式 1.2 |
| szcom cache_ | 无字节预算驱逐（跨批累计无上界） | 与 materializeBatch 共享预算的 LRU | 所有权 F5 |

---

## 6 审计详录（材料库）

> 本节为四路审计的发现详录与 file:line 证据，按需查阅；已被 §4/§5 吸收的项不重复展开。

### 6.1 现代 C++ 审计

- **高危 H1-H4**：已提升至 §4（D1-D4）。
- **中危（M 级）**：
  - M1 手工资源未 RAII：sink.cpp:230-291 裸 HANDLE 两段 catch；szcom COM 十余处
    手工 Release（nxshell 同）；main write_report_file 短写未查；log probe 句柄
    ——随批次 5（res/ 圈禁）处理；
  - M2 filter.cpp 五解码器 init/end 手工清理：20+ 出口点重复释放，五段多成员重启
    逻辑同构 ~180 行——RAII guard + 模板合并，**收益最高单点**（~100-150 行净减）；
  - M3 readEntryData/readEntryDirect ~40 行错误处理逐字重复 → 抽 pullBlock()；
  - M5 错误体系：异常层次保留（跨线程 exception_ptr 是正当场景）；`OpenOutcome`
    即手写 expected，VS2026 一行切 `/std:c++23`（vcpkg C 库 ABI 无影响）；
  - M6 密码边界残留：szcom.cpp:337/463 经未擦除临时拷贝；gui.cpp:178 无擦除；
  - M8 printf/varargs 日志 → `std::format` 渐进迁移；log.cpp:79 三目两分支相同笔误。
- **低危（L 级）摘选**：stop_token 两处虚设；ThreadPool::submit started_ 无锁；
  parse_size 死代码；死成员若干；nextAttempt 每次物化候选 vector；TplBuilder
  reinterpret_cast 无边界检查；void* handle_ 类型擦除；GetTickCount64 vs chrono。

### 6.2 领域建模审计（14 项）

见 §5.3 表（全部项已收录，含工作量估计：速赢项 0.5-1d，LayerPath/Walker 1.5d，
VolumeGroupBuilder 2-3d，engines 拆分 2d 等）。评估结论：ByteSource 四方法接口
**暂缓拆分**（热路径 D5 零拷贝优先，仅升格契约文档）。

### 6.3 函数式化审计

- **全局状态**：g_prog（gui.cpp:230，fuzz 靠隐式 no-op 路径）、log 四全局（g_quiet
  竞态→D5）、g_nameCp（engines.cpp:26，隐式历史依赖）、szcom DLL 五全局
  （→函数局部 static 单例）。测试开关经环境变量渗入产品的路径不存在（健康）。
- **纯化清单**（P1）：detect 拆 `detect_from_bytes`（纯）+ peek 壳 + `valid_zip_lfh`
  去重；stego 拆 `parse_atom_header` / `eocd_from_window`；report 拆 snapshot+
  render；volumeset 拆 `select_group`；main 参数解析提 `parse_args → expected`
  （消灭 need() 里的 std::exit）；CbCtx→AccessRecorder；`ascii_lower` 统一 7 处；
  split_ext 双实现合一；handle_branch_error 拆纯决策 classify_branch。
- **错误传播分级**：纯解析/探测 → `std::expected`（C++23 一行切换，评估可行低风险，
  先 `Result<T> = std::expected<T,std::string>` 别名隔离）；**保持异常**：跨线程
  exception_ptr、中止/熔断/密码耗尽控制流（8 层递归穿透）、C 回调边界。
- **ranges 红线（明确不做）**：filter 五泵（双向差分状态同步+有序释放）、
  readEntryDirect（span 生命周期）、PushbackSource（性能关键）、writeOne（DWORD
  分块纪律+熔断交织）、SFX/EOCD 扫描体（反向带索引无惯用等价）——保留命令式体、
  只纯化接口。冷路径 6 处可换（JSON 拼接/保留名查找/printable4 等）。

### 6.4 所有权与环审计（C1-C10 / F1-F5）

- 穷举 95 节点 50 边（原文档含完整边表，重构时按基线 f647037 复核）；
- **C1 = replayQ_ 环**（唯一真实环，→ D6）；C2 SevenZipReader 无环但距复现一步
  （若加预取队列即成环——AST 检查器防的就是这个模式）；C3 cache_ 无驱逐（F5）；
  C4 abandon 协议正确但 F4 顺序窗口；C5/C10 Sink 裸回边+析构序 UB 风险（idle 断言）；
  C6-C9 全部验证通过（gui 线程/CbCtx 对象内自洽/COM 契约/RegionView 链）；
- **归约定理**：unique_ptr 与 owning-raw 字段全部构造点单向注入 ⇒ 无环性 ⇔
  shared_ptr 子图无环 ⇔ 任一 esft 类的"成员强闭包"不含自身——环检查可静态化。

---

## 7 形式化验证方案

### 7.1 不变式

| 编号 | 陈述 |
|---|---|
| INV-1 | 所有权图 (V, ≺) 无环（由归约定理，检查 esft 类成员强闭包即可） |
| INV-2 | ∀ 活跃对象从根集可达（根集 = Session ∪ Walker 栈帧 ∪ Sink 任务 ∪ 7z.dll COM ∪ 进度线程）——违反即泄漏 |
| INV-3 | 读取器成员容器不得持有"条目源或含条目源的类型"（唯一合法父方向强边的对偶） |
| INV-4 | 借用先于亡（四条纪律逐条断言化，见 §5.1） |

### 7.2 运行时哨兵（diag 模块 ~150 行，`#ifdef NX_DIAG_LEAKS`，fuzz 常开）

| # | 对象 | 断言 | 抓什么 |
|---|---|---|---|
| S1 | SpoolBuffer 注册表（weak_ptr+构造点） | 退出/每迭代断言全灭，违反 abort | 任何 spool 泄漏 |
| S2 | ContainerReader 注册表 | 同上 | 读取器级泄漏 |
| S3 | try_open 失败出口 | 本次创建的 reader 已析构（进出计数差 0） | **案发现场**直接抓 D6 触发族 1 |
| S4/S5 | ~ThreadPool / ~Sink | `q_.empty() && running_==0` | C5/C10/F1 |
| S6 | LaSeqReader dtor | replayQ_ 空（修复后结构恒真，留作回归丝） | D6 退化 |
| S7 | fuzz 每迭代 | 句柄计数差 ≤ 阈值 / tempDir 无 *.tmp | 句柄/文件泄漏 |
| S8 | COM 四实现类 | 全局 add/release 差额迭代末为 0 | 7z.dll 意外保留回调 |
| S9/S10 | 进度线程重入 / 退出转储 | — | 兜底 |

### 7.3 静态检查

- **AST 强闭包检查器**（`nx-no-self-strong-closure`，零维护形态：
  `clang-cl -Xclang -ast-dump=json` + Python 不动点闭包，并入
  `tests/audit_ownership.py`，~200 行）：esft 类成员强闭包不得含自身。
  **校准标准：对基线 f647037 运行须恰好报 LaSeqReader 一处、零误报**（能在既有
  代码上精确复现已知事故且零误报，是检查器可信度的直接证明）。已知盲区（lambda
  捕获 `self`/`&]` ~6 处）用 grep 白名单补。
- clang-tidy 推荐集：`cppcoreguidelines-owning-memory`（+gsl::owner 标注驱动
  res/ 迁移）、`bugprone-dangling-handle`、`concurrency-mt-unsafe`（把 g_nameCp/
  g_prog/g_dll 变显式债务清单）、`performance-unnecessary-value-param` 等；
- MSVC `/analyze` 低噪子集顺手排雷（对环零检出能力，不作主线）。

### 7.4 模型检查（分级）

| 手段 | 对象 | 时机 | 成本 |
|---|---|---|---|
| **TLA+ / TLC** | 所有权小模型（已落地：tools/ownership.tla 三变体 + 三个 cfg；run_tests `ownership_tla` 用例为**硬门**——缺 jar/java 直接 FAIL） | **批次 4 的前置验收门（已过）**：legacy 复现 replayQ_ 反例（TLC 完备反例 trace = Open→ProbePush→DropReader）、weakOnly 证"weak 与 KeepAlive 必须配套"（AsyncNoUseAfterDead 违例）、fixed 全状态空间零违例 | 已完成 |
| TLA+ | BoundedQueue abandon 协议（NoDeadlock/NoLostWake） | 工具已就绪（tla2tools.jar），随批次 6 或单独排期 | 0.5d |
| CBMC | regionOf 区间扫描抽取纯函数 / stego 边界 / sanitize_segment（下标越界·回绕·接受谓词） | 随批次 5/6 顺手（抽取即单测） | 0.5-1d/函数 |
| F*（已装：tools/fstar 2026.09.27） | AST 强闭包检查器的正确性证明（闭包定点 = 数学可达性、终止性）——检查器本体仍是 clang-cl ast-dump + 程序，证明其算法 | 批次 5 | 1-2d |

---

## 8 执行计划（单一批次制）

> v1 中 Phase 0-3 与审计批次 B1-B6 两套体系已合并去重。总量约 **5-7 周**人工。
> 每批独立全量回归：49 属性 + 9 GUI + fuzz（哨兵常开）+ 真实样本端到端（15GB 隐写 ×2 哈希比对）。

| 批次 | 内容 | 来源 | 量 | 验收门（增量） | 状态 |
|---|---|---|---|---|---|
| **0 缺陷修复** | §4 D1-D8 全部（D6 环修复 + D7 过滤器链深度 + S1-S5 哨兵随行） | §4 | ~2.5d | §4 验收门 | ✅ **已完成**（2026-10-02，36d04d1…8dfe638；49/49 + 9/9 + fuzz 哨兵常开） |
| **1 低风险速赢** | FormatInfo 表 / NameCodec 会话 / Outcome+derive_exit_code / AccessRecorder / Detection.note / ascii_lower 统一 / detect·stego·report·volumeset 纯化 / SafePath / C++23 切换 + Result 别名试点 / M2 filter RAII 化 / M3 pullBlock | 领域 #1/3/4/5/12 + 函数式 P1 + M2/M3 | ~1 周 | 新增单测（纯核心）+ 全量 | ✅ **已完成**（2026-10-02，836a435…；nxunit 235 检查 + 50 属性 + 9 GUI。volumeset 的 select_group 拆分顺延批次 3（单测壳就绪后一并接入），其余全量落地） |
| **2 行为敏感** | LayerPath/LayerCtx + Walker 对象化（**密码缓存键语义修正**：深度+basename → 逻辑路径） | 领域 #2/11 | 1.5d | 密码专项回归 + 全量 | ✅ **已完成**（2026-10-02，两步提交：LayerId key/display 拆分 + join_logical 逻辑路径键——修复真实 bug：不同父容器同名分片组共享"深度+名"键，a 组耗尽候选污染共享游标 → b 组假性 PasswordExhausted（0 文件）；LayerCtx 收敛 walk 散参数 + Walker 类 + resolve_runtime_options 装配单点。语料 sibling_pw_cache + nxunit 251 + 51/51 + 案例 L 15GB 复验） |
| **3 可测性** | PromptSink / MemorySource / 单测壳接入（消毒器·密码链·分片分组·退出码·detect） | 领域 #7 + 测试性 | ~3d | C++ 单测首批入套件 | ✅ **已完成**（2026-10-02：MemorySource 内存源 + 过滤器泵/detect 壳真实路径集成测试；PromptSink 脚本化提示（密码链全语义可单测）；select_group 拆分（group_filesystem 只剩 IO 壳）+ volumeset 三层单测。消毒器/退出码/detect 已随批次 1 落地。nxunit 298 检查，51/51） |
| **4 契约与所有权** | **先过 Alloy 验收门** → EntryToken 契约 + weak_ptr/KeepAlive + 视图合并（make_* 三工厂 + MeteredViewFactory）+ engines 拆分五文件 + szcom cache_ 预算驱逐 | 领域 #9/10/13 + Phase 1 + F5 | ~1.5 周 | S1-S3 哨兵全绿 + 全量 | ✅ **已完成**（2026-10-02。验收门以可执行穷举模型检查器落地（tests/ownership_model.py，对象≤6/深度≤12 全序列）：legacy 复现 replayQ_ 反例（open→probePush→failOpen）、weakOnly 证"两者必须配套"、fixed 零违例；代码：weak_ptr 条目源 + ByteSource::keepAlive() 令牌（Sink 任务提交时捕获）、EntryToken 契约、views.{hpp,cpp} 唯一实现 + ViewFactory 挂表纪律类型化、engines.cpp 拆五件（namecodec/laimp/zipcd/laseq/open）、szcom cache_ 共享预算 LRU 驱逐（中读豁免）。S1-S5 哨兵诊断构建全静默 + 52/52 + fuzz + 15GB 复验） |
| **5 资源圈禁** | res/（UniqueFile/TempFile/com_ptr）全量迁移 + gsl::owner 标注 + audit_ownership.py（grep + AST 闭包检查器，校准标准 §7.3） | Phase 2 + M1/M4 | ~1 周 | 审计脚本零违规 + 全量 | ✅ **已完成**（2026-10-02。AST 检查器先行落地并校准（f647037 恰报 LaSeqReader 零误报，run_tests 硬门 53/53）；随后 res/ 圈禁全量：`UniqueHandle<T,Invalid,Closer>` header-only 模板 + UniqueFile/UniqueRegKey/UniqueModule（文件/注册表/DLL 三类 Win32 句柄——含 7z.dll 探测期 UniqueModule 自动卸载、命中后 owner 转移进程缓存）、TempFile 工厂（DELETE_ON_CLOSE 统一）、com_ptr（szcom+nxshell 全部手工 Release）、DeleteGuard（sink .part 两段 catch 归一）；audit_ownership.py 增 grep 圈禁硬门（剥注释，负样本自测）；gsl::owner 标注三个逃逸点（res/gsl_owner.hpp 零依赖自带）；M1 顺手修复 write_report_file 短写静默。**两个实证教训**：①DeleteGuard 声明序=先关句柄后删文件（反序被 GUI 用例 6 抓住）；②COM 释放顺序契约——Open 失败后 7z.dll 仍持流引用，arc.reset() 必须先于 mainStream_.reset()（反序 heap-use-after-free，fuzz ASan 抓获，工件入库为回归种子）。nxunit +27=325，53/53 + 9/9 + fuzz 全绿） |
| **6 验证常态化** | 哨兵 fuzz 常开 + clang-tidy CI 基线 /analyze 子集 + AGENTS 增"所有权纪律"一节 + 真实样本发布门固化 | Phase 3 | ~1d | CI 全绿 | 待排期（哨兵 fuzz 常开已随批次 0 落地；批次 5 的 fuzz 种子库已含 COM 顺序回归工件） |

**顺序依赖**：0 独立可发布 → 1/2/3 可并行排期 → 4 依赖 1（C++23/Result）与 2
（LayerCtx）→ 5 依赖 4（视图合并先行）→ 6 收尾。

---

## 9 决策记录

| # | 日期 | 决策 | 背景 |
|---|---|---|---|
| D-1 | 2026-10-02 | **递归深度默认 10 层，过滤器链纳入 `--depth` 约束**。实现草案：`walk()` 增 `filterChain` 参数（容器分支重置为 0，过滤器分支 +1 并检查超限抛 LimitError/exit 3）；`maxDepth` 默认 8→10 + usage 文案；depth_bomb 语料加深至 12 层；新增 filter_depth_bomb 用例（30 层嵌套 gzip，期望退出码 3） | F2/DoS：过滤器分支同 depth 递归无上界，4MiB 输入可构造数千层嵌套 gzip 致栈溢出/线程耗尽。默认 10 层已覆盖全部真实需求（用户确认）。注意：不能让过滤器直接计入 `depth`——会破坏 tar.gz 根的 noRoot 语义与层编号 |
| D-2 | 2026-10-02 | **形式化工具选型：TLA+/TLC（模型检查）+ F\*（证明）**（用户指示"验证器用成熟的，不要自制简陋模型"；门就是门，CI 不允许静默跳过）。TLC 完备检查为唯一权威门并硬性接入 run_tests（缺 jar/java 直接 FAIL，`ownership_tla` 用例）；自制 Python 穷举器**已删除**不再维护。所有权小模型 tools/ownership.tla 三变体共用一份规范，GC 为独立原子 Collect 动作。证明侧 F\* 2026.09.27 已就位（tools/fstar/，用户配置），链路自检 tools/proofs/Smoke.fst（闭包保种子/单调性引理全由 z3 自动 discharge） | 批次 4 验收门最初以自制 Python 穷举器达成，用户两次纠正：①自制模型只配当校准脚本不配当权威验证；②"缺 jar 时优雅跳过"不是门——强制要求通过 |
| — | — | 待决事项：F\* vs Coq 二选一（证明侧工具，用户配置） | — |

---

## 10 附录

### 10.1 审计元信息

四路独立 agent 审计（2026-10-02，基线 f647037）：现代 C++ 合规 / 领域建模与对象
抽象 / 函数式化与副作用治理 / 所有权图建模与形式化验证方案。全量源码 40 文件
~6700 行，只读审计。所有权审计穷举 shared/unique 节点 95 处、边 50 条。

### 10.2 与既有文档的关系

- 本文档是**重构与现代化**的权威计划；日常进度与已知问题仍以 README 为准；
- AGENTS.md 将在批次 6 增补"所有权纪律"一节（P1-P6 的操作化清单）；
- 设计文档 §4 将增补所有权图谱与生命周期状态机（批次 4 交付物）。
