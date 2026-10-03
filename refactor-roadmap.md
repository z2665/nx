# nx 现代化重构路线图（refactor-roadmap）

> **状态：批次 0-6 全部完成**（2026-10-02/03，36d04d1…5277a7e）。基线 f647037。
> 本文档现为**完成记录与纪律权威**——§2 硬边界、§3 原则 P1-P6、§5 现行架构、
> §6 验证体系、§8 决策记录持续有效；逐批执行细节见各 commit 与 README。
> **配套**：[README.md](README.md)（进度权威）· [AGENTS.md](AGENTS.md)（工作区纪律，
> 含所有权纪律操作化清单）· [nested-extractor-design.md](nested-extractor-design.md)（设计权威）

---

## 1 背景与动机（摘要）

三个 15GB 级真实案例（代号 L/XJ/M，结构与教训见 README「稳定性修复」）暴露
正确性/健壮性/性能三维度债务。根因定案最深一处：**LaSeqReader replayQ_ 自引用环**
——esft 类型把 `shared_from_this()` 交给"将被自己持有的结构"＝制造环：失败尝试
的读取器永不析构 → spool/视图连带泄漏（15GB 临时文件残留案例）。教训升格为全局
纪律（P1 + AGENTS 所有权纪律节）。

四路独立审计（现代 C++ / 领域建模 / 函数式化 / 所有权形式化，2026-10-02）交叉
印证：firstHardError 竞态、g_nameCp/g_prog 隐式全局、两套 seek 视图逐行重复、
95 节点 50 边所有权图中唯一真实环 = replayQ_。审计材料在规划期全部吸收
（D1-D8 / §5 架构 / 各批次内容），详录不再保留于本文档。

## 2 目标与非目标

### 2.1 目标（全部达成）

1. 所有权关系可证明无环（TLA+ 双模型 + AST 强闭包检查器 + F* 证明，§6）；
2. 一切手动释放资源圈禁到 `src/res/`，其余代码只见 RAII 类型（P2）；
3. 领域概念显式类型化，消灭平行重复实现（§5.3）；
4. 纯核心/效果壳分离——决策逻辑确定性可单测（nxunit 325 项）；
5. 所有权与关键协议有形式化模型与验证手段（§6）。

### 2.2 非目标 / 硬边界（持续有效）

- **行为语义零变化**：免 spool 直读及其回退语义、密码解析链顺序、深度/总量/
  压缩比/磁盘水位熔断、退出码契约（0/1/2/3/4/64）、`.part` 原子落名——除非经
  §8 决策显式变更；
- **性能红线**：filter 五解码泵、readEntryDirect 块交付状态机、PushbackSource、
  writeOne 写出循环、SFX/EOCD 扫描体不做纯度牺牲（ranges 化红线）；
- 密码纪律：绝不入日志/报告，SecureStr 边界只收紧不放松；
- 热路径零拷贝（直通视图）不可引入间接层。

## 3 设计原则（P1-P6，持续有效）

| # | 原则 | 含义 |
|---|---|---|
| P1 | **所有权单向流动** | Walker → ContainerReader → ByteSource 树；唯一合法"父方向"强边是条目源→读取器，前提是读取器成员永不回存条目源 |
| P2 | **泄漏调用圈禁** | Win32 句柄 / COM 引用 / 临时文件只允许出现在 `src/res/`，其余代码经 RAII 类型使用 |
| P3 | **泄漏可观测** | 常驻哨兵 + 静态审计脚本 + fuzz 断言，回归即抓 |
| P4 | **值语义与 RAII 优先** | 消灭裸 new/裸句柄/手工清理；输出参数换返回值；错误按值表达 |
| P5 | **纯核心 / 效果壳** | 解析、推导、打分、消毒、决策为纯函数；IO/日志/GUI/线程集中为显式实例的壳 |
| P6 | **不变式显式化并可验证** | 写成断言/检查器/模型，不留在注释里 |

## 4 缺陷登记簿（D1-D8，全部修复 2026-10-02）

| # | 问题 | 修复 |
|---|---|---|
| D1 | 命令行密码明文入 nx.log | argv 红线过滤（36d04d1） |
| D2 | `--depth abc` → std::terminate（违退出码契约） | from_chars 全量校验 → 64（029b8da） |
| D3 | firstHardError 跨线程无锁读写 | HardErrorSlot 自带互斥（1e90f08） |
| D4 | win_long_path 超长采纳未初始化栈缓冲 | 两次调用协议（0f8e937） |
| D5 | g_quiet 数据竞争 | std::atomic（33b4c8c） |
| D6 | **replayQ_ 自引用环**（§1） | 重放队列只存元数据，重放时重建条目源（44503e3） |
| D7 | 过滤器链递归深度无上界（DoS） | 链纳入 `--depth`，默认 8→10（12ec147，决策 D-1） |
| D8 | 小项合集（死代码/构造顺序 OOM 窗口/`.Z` 匹配/note 绕过日志） | 09d8850 |

随行落地：哨兵 S1-S5（8dfe638，fuzz 常开）；校准实验——带环旧实现 + 前置目录
加密 zip 无密码 → S3 案发现场 abort，修复后同输入 exit 2 静默。D6 的 dtor 断言
（S6）未做：重放队列类型层面已不可能持条目源，编译期保证强于运行时断言。

## 5 现行架构（目标已全部落地）

### 5.1 所有权模型

强所有权边构成以 {main 栈 Session、Walker 活动栈帧、Sink 任务队列、7z.dll COM
引用} 为根的 **DAG**。核心机制：

- **weak_ptr + KeepAlive 令牌**（配套落地，weakOnly 变体为 TLA+ 反例已证形态）：
  条目源（LaEntrySource/EntrySource）对读取器只持 `weak_ptr`；异步写出的存活由
  Sink 任务捕获 `keepAlive()` 保活令牌；
- **生命周期状态机**与关键不变式：

| 类型 | 状态机 | 关键不变式 |
|---|---|---|
| SpoolBuffer | growing → finished（overflowed 单调） | read_at 仅 finished；append 仅 growing；overflowed ⇒ ram_.empty() |
| LaSeqReader | probing → iterating → drained → dead | **INV-REPLAY：dead 时 replayQ_ 必空**；adoptStream 至多一次 |
| SevenZipReader | opening⇄enumerate⇄probe → ready → iterating → dead | arc_≠null ⇒ ready/iterating；cache_[i].pos 单调 |
| Sink/ThreadPool | accepting → draining → destroyed | **INV-SINK：destroyed 仅可自 draining 迁入**（先 waitAll） |
| BoundedQueue | open/closed → dead（终态） | dead 时两侧必不阻塞（TLA+ DeadRelease 已证） |

- **借用纪律断言化**（四条）：Sink 析构前必 waitAll；QueueSource 先亡于队列
  （F4 顺序修正）；Session/Sink 成员声明序显式化；COM 释放契约（S8 哨兵）。

### 5.2 模块划分

```
src/
  res/        ← P2 圈禁：UniqueHandle 模板 + UniqueFile/UniqueRegKey/UniqueModule、
               │  TempFile（DELETE_ON_CLOSE 唯一工厂）、com_ptr、DeleteGuard、
               │  gsl_owner（零依赖自带）；文件句柄接入一律 res::adopt_file()
               │  释放五名单（CloseHandle/DeleteFileW/RegCloseKey/FreeLibrary/
               │  ->Release()）以 tests/audit_ownership.py 的 RELEASE_CALL 为唯一权威
  纯核心      ← P5：detect_from_bytes / parse_atom_header / eocd_from_window /
               │  charset_candidates / sanitize·NamePolicy / ascii_lower /
               │  group_volumes·select_group / render_report / derive_exit_code
  效果壳      ← IO 对象（FileSource/FileSeekView/SpoolBuffer/RegionView 唯一实现，
               │  ViewFactory 挂表纪律类型化）；进程单例（Logger/SzDll）；
               │  会话实例（ProgressUi/PasswordProvider）；编排壳（walker/sink/main）
  engines：namecodec / views / laseq / zipcd / open（自 engines.cpp 拆分五件）
```

名单外资源（fclose/LocalFree/CoTaskMemFree/archive_read_free）依决策 D-3 豁免。

### 5.3 领域类型（均已落地）

`kFormatTable`+`format_info()`（格式知识唯一事实源）· `LayerId/LayerCtx`（层身份，
密码缓存键=逻辑路径——修复兄弟分片组共享游标真 bug）· `EntryToken`（迭代契约，
重放只携带 {token,meta}）· `AccessRecorder`（数据相位私有状态机）· `NameCodec`
（每读取器一份）· `sanitize_rel`+消毒纯函数 · `VolumeGroupBuilder`/`select_group`
（分片分组三层单测）· `PromptSink`（密码链脚本化单测）· `Outcome/derive_exit_code`
（退出码契约单测）· `Walker/resolve_runtime_options`（装配单点）· `Detection::note`
（检测注记结构化）· `MemorySource`（管线免文件系统测试）· szcom cache_ 共享预算
LRU（中读豁免）。

## 6 验证体系（现行，全部落地）

### 6.1 不变式

| 编号 | 陈述 |
|---|---|
| INV-1 | 所有权图无环（检查 esft 类成员强闭包即可——归约定理） |
| INV-2 | ∀ 活跃对象从根集可达（违反即泄漏） |
| INV-3 | 读取器成员容器不得持有"条目源或含条目源的类型" |
| INV-4 | 借用先于亡（四条纪律断言化，§5.1） |

### 6.2 运行时哨兵（src/diag.hpp，`NX_DIAG_LEAKS`，fuzz 常开；诊断构建 build-diag.cmd）

S1/S2 spool/读取器活性注册表（退出全灭断言）· S3 try_open 失败出口守卫（D6 案发现场）
· S4/S5 ~ThreadPool/~Sink 析构纪律 · S7 句柄/临时文件迭代差 · S8 COM 四实现类
余额 · S9/S10 进度线程重入/退出转储。（S6 见 §4 注。）

### 6.3 静态检查

- **AST 强闭包检查器**（`tests/audit_ownership.py`）：clang-cl ast-dump →
  类→成员强边表（shared_ptr/unique_ptr 派生展开；容器元素计强边；weak/裸指针
  不计）→ **F\* 验证 + KaRaMeL 抽取的 closure_check.exe** 判定 esft 类成员强
  闭包含自身。信任链：Closure.fst 四引理全 VC → C → exe（build_closure_kernel.cmd）。
  校准：f647037 恰报 LaSeqReader 零误报；HEAD 零违规；fail-loud（TU 解析失败
  即败，同 HEAD 判定字节级确定）。
- **P2 圈禁 grep 门**（同脚本）：五名单越出 res/ 即 FAIL；每次运行先跑正/负
  样本自检。已知边界：点调用/函数指针/宏间接不匹配（当前代码无此形态）。
- **clang-tidy 基线门**（`tests/tidy_check.py` + `.clang-tidy`）：owning-memory 等
  检查零警告（语义边界 NOLINT 附理由，定界见 D-3）。
- **MSVC /analyze 排雷**（`build-analyze.cmd`，低噪子集，非门）：项目源零警告。

### 6.4 模型检查与证明（run_tests 硬门：缺 jar/java 直接 FAIL）

| 手段 | 对象 | 状态 |
|---|---|---|
| TLA+/TLC `tools/ownership.tla` | 所有权三变体（legacy 复现 replayQ_ 反例 / weakOnly 证"weak 与 KeepAlive 必须配套" / fixed 零违例） | ✅ `ownership_tla` 门 |
| TLA+/TLC `tools/boundedqueue.tla` | BoundedQueue abandon 协议三变体（closeNoWake/abandonNoWake 校准反例必违 / fixed 零违例零死锁：DeadRelease + ParkedSanity） | ✅ `boundedqueue_tla` 门 |
| F\* `tools/proofs/Closure.fst` | 闭包检查器算法正确性（P0-P3 四引理全 VC → KaRaMeL C） | ✅ 抽出为判定内核 |

三层分工：**TLA+ 管设计语义、AST 检查器管代码现状、F\* 管检查器不说谎**。

## 7 执行记录（批次 0-6 全部完成）

| 批次 | 交付（详 commit） | 关键点 |
|---|---|---|
| 0 缺陷修复 | D1-D8 + 哨兵 S1-S5 | 49/49 + 9/9 + fuzz 校准 |
| 1 速赢 | filter RAII 化/pump_members、pullBlock、纯函数抽离六件、领域类型首批、C++23+Result、nxunit 单测壳 | 235 项 |
| 2 行为敏感 | LayerId key/display 拆分（密码缓存键=逻辑路径，修兄弟分片组真 bug）+ LayerCtx + Walker 类 | 语料 sibling_pw_cache |
| 3 可测性 | MemorySource 集成测试、PromptSink 脚本化密码链、select_group 三层单测 | nxunit 298 |
| 4 契约与所有权 | TLA+ 前置门、weak_ptr+KeepAlive、EntryToken、views 唯一实现+ViewFactory、engines 拆五件、szcom LRU | 52/52 + 15GB 复验 |
| 5 资源圈禁 | res/ 全量迁移（四头文件）、AST 强闭包检查器（F\* 链）、圈禁 grep 门、gsl::owner、M1 短写修复 | 53/53；三路评审修复：UniqueFile 哨兵 nullptr 化（NTTP 合法性）、audit fail-loud、fuzz 回归种子入库 |
| 6 验证常态化 | clang-tidy 基线硬门、/analyze 零警告、合成发布门（gen_release_corpus + release_gate，D-4）、AGENTS 所有权纪律节、文档匿名化、BoundedQueue TLA+ | 55/55 |

每批验收：全量回归 + GUI 9/9 + fuzz（哨兵常开）+ 发布门（批次 6 起）。

## 8 决策记录

| # | 日期 | 决策 | 背景 |
|---|---|---|---|
| D-1 | 2026-10-02 | **递归深度默认 10 层，过滤器链纳入 `--depth` 约束**（容器分支重置链计数；不能让过滤器直接计入 `depth`——破坏 tar.gz 根的 noRoot 语义与层编号） | F2/DoS：4MiB 嵌套 gzip 可致栈溢出。默认 10 层覆盖全部真实需求（用户确认） |
| D-2 | 2026-10-02 | **形式化工具选型：TLA+/TLC（模型检查）+ F\*（证明）**——验证器用成熟的，不自制简陋模型；门就是门，缺工具直接 FAIL 不静默跳过 | 用户两次纠正：自制模型只配当校准脚本；"优雅跳过"不是门 |
| D-3 | 2026-10-03 | **圈禁名单定界五项；CRT/内存分配器族显式豁免**（fclose/LocalFree/CoTaskMemFree/archive_read_free——内存释放非 Win32 句柄/COM/临时文件，散布增长再收编）。NOLINT 边界定界：Win32 API 返回值接入与 COM out 参数引用计数移交属"无标注世界 ↔ owner 纪律"边界；sink 参数 move 惯用法、jthread stop_token 惯例同理 | 批次 5 评审 M-5；tidy 首跑 13 位点即此三类边界 |
| D-4 | 2026-10-03 | **测试语料隐私纪律：真实样本不入仓**——所有测试样本自建（含加密/不加密、密码可控），基于真实样本的结构构建；文档历史案例以代号引用（L/XJ/X/Z/M/N） | 用户明确：真实文件名/路径/密码泄露用户信息 |
