# nx 开发指南（DEVELOP）

> 面向开发者与贡献者：构建、源码地图、领域模型、CI 硬门与红线。
> 面向用户的说明（功能/安装/用法）见 [README.md](README.md)。
> AI 协作代理请先读 [AGENTS.md](AGENTS.md)（文档阅读顺序与 plan/ 工作目录约定在那里）。

**文档地图**：[nested-extractor-design.md](nested-extractor-design.md) = 设计权威（领域模型/架构/所有权/全部决策）；
本文件 = 怎么开发；[AGENTS.md](AGENTS.md) = 工作区纪律与红线；`plan/`（本地，gitignored）= 进度与计划。

## 构建

```cmd
build.cmd            # CMake + Ninja + VS 2026（vcvars64）+ vcpkg → build\nx.exe（约 5.5 MB 单文件，仅系统 DLL 依赖）
package.cmd          # 便携打包 → dist\nx\（nx.exe + 7z.dll(可选) + 文档；需先 build.cmd）
build-analyze.cmd    # MSVC /analyze 排雷（低噪子集，非门）
build-diag.cmd       # 诊断构建：泄漏哨兵 S1-S5 全开（NX_DIAG_LEAKS_MAIN）
```

注意：`build.cmd` 硬编码了本机代理 `127.0.0.1:10808` 与 `VSROOT`（VS 2026 路径）——换机器需改。
git-bash 下调用须写 `cmd //c build.cmd`（`/c` 会被 MSYS 路径转换吃掉，导致假成功）。

### 依赖（vcpkg manifest 固定版本）

libarchive 3.8.7（容器）+ **zlib-ng[compat]**/bzip2/liblzma/zstd/lz4（过滤器直连），
triplet `x64-windows-static`，`/MT` 静态 CRT。两个必须的 overlay：

- `ports-overlay/zlib-ng`：基线端口无 feature，自建 `compat`（ZLIB_COMPAT 构建，导出标准 ZLIB 配置供三方经 `<zlib.h>` 链接）。
- `ports-overlay/libarchive`：①上游 CMake 未链 crypto 探测 `PKCS5_PBKDF2_HMAC_SHA1`，WinZip AES 走了 stub——强制定义修复；②`nx-batch-ctr.patch`：上游 WinZip AES 每 16 字节一次单块 EVP（~60MB/s），批量化为每 64KiB 一次（AES 2GiB 实测 33.6s→2.7s，内容校验一致；曾按上游风格提交 PR libarchive#3443，上游暂无 review 带宽已关闭，overlay 持续生效）；③配套上游风格 round-trip 测试补丁。

7z.dll 为**运行时按需加载**（exe 目录 → 7-Zip 安装目录 → PATH），负责 7z 全特性（AES+头加密+原生分卷）与 RAR 解码；缺失回退 libarchive（RAR 多卷除外）。`szcom.cpp` 是其唯一适配层。

## 源码地图（文件 → 模块 → 职责）

依赖方向严格单向（DAG），按层列出；**勿跨层直达**（分层图与红线见 AGENTS 架构分层节）。
登记例外：`gui.hpp` 的进度/取消/密码弹窗为横切面，允许领域/编排层调用其自由
函数（未显示时廉价 no-op，单二进制形态下零间接层；未来 GUI 壳拆分时需引入通知端口）。

| 层 | 文件 | 职责 |
|---|---|---|
| 基座 | `util.hpp/cpp` | 编码转换、Win32 路径（`\\?\` 长路径/递归建目录）、尺寸解析 |
| | `outcome.hpp` | 错误分类学（Error/Limit/Password/MissingVolumes/Corrupt/Cancelled）+ `Result` 别名 + 退出码纯推导 |
| | `diag.hpp/cpp` | 泄漏哨兵 S1-S5（`NX_DIAG_LEAKS`，fuzz 常开；泄漏=abort） |
| | `log.hpp/cpp` | 日志体系 + 密码红线过滤 + 默认 nx.log（5MiB 截断） |
| | `namecodec.hpp/cpp` | 条目名码表修复（每读取器粘性） |
| 领域 | `format.hpp` | 格式枚举 + `kFormatTable` 单一事实源（格式→类属→名称）+ 过滤器后缀剥离 |
| | `password.hpp/cpp` | SecureStr 安全擦除、LayerId（key=逻辑路径/display=提示）、PasswordProvider 解析链、PromptSink 测试注入 |
| | `pipes.hpp/cpp` | BoundedQueue（close/abandon 协议，TLA+ 验证）+ ThreadPool |
| | `volumeset.hpp/cpp` | 分片命名识别（拼接型/zspan/RAR 原生）+ 完整性预检（纯函数可单测） |
| | `stego.hpp/cpp` | 隐写检测纯核心：MP4 atom 步进 + EOCD 反扫（CD 签名自证） |
| | `layer.hpp` | LayerCtx 递归上下文（sub/origin/chain/logical/depth/filterChain + 帧工厂） |
| | `session.hpp` | Options / Stats / HardErrorSlot（会话聚合根 Session 在 walker.hpp，见其头注） |
| 流 | `bytesource.hpp/cpp` | ByteSource 唯一流抽象（read/read_direct/sizeHint/seekRegion/keepAlive）+ File/Concat/Null/Memory/Shared/Queue 简单源 + RegionSource 契约 + InputMeter |
| | `pushback.hpp/cpp` | PushbackSource 回看流（peek/rewind/直通三模式）——管线通用适配器 |
| | `res/` | **资源圈禁区（P2）**：UniqueFile/UniqueRegKey/UniqueModule/TempFile/com_ptr/DeleteGuard（header-only；释放五名单不得出 res/） |
| 引擎基座 | `container.hpp` | ContainerReader 契约 + EntryToken（迭代身份）+ EngineOptions + VolumeSource（卷数据三态） |
| | `detect.hpp/cpp` | 内容嗅探（纯核心 `detect_from_bytes` + 流式包装） |
| | `filter.hpp/cpp` | 五解码器直连泵（多成员串联窥探重启、FilterLimiter 压缩比熔断） |
| | `spool.hpp/cpp` | SpoolStore：RAM 自适应 → 磁盘溢出（DELETE_ON_CLOSE、≤16MiB 分块、ioM_ 并发） |
| 视图 | `views.hpp/cpp` | SeekView 三态（File/Spool/Region 可链式套窗口）+ ViewFactory（InputMeter 挂表纪律类型化） |
| 引擎实现 | `laimp.hpp` | libarchive 内部共享件（错误分类/AccessRecorder/回调）——勿在公共头引用 |
| | `zipcd.hpp` | zip 文件名码表探测（候选逐一试开择优） |
| | `laseq.hpp/cpp` | LaSeqReader 顺序读取器（probe/重放队列只存元数据/`regionOf` 区间推导） |
| | `szcom.hpp/cpp` | 7z.dll COM 适配（IInArchive、多卷回调、双通道密码、materializeBatch solid 批量抽取、cache_ LRU） |
| | `open.cpp` | **engines 门面实现/组合根**：try_open 探测、密码迭代、spool 兜底、zip 中央目录、R 类 7z.dll 优先与回退 |
| 编排 | `engines.hpp` | 容器引擎门面（open_container / open_container_volumes / open_zip_file / sevenzip_dll_available） |
| | `sink.hpp/cpp` | 安全落盘：路径消毒（sanitize 纯函数）、大小写重名登记、`.part` 原子落名、写出线程池、sha256 校验 |
| | `walker.hpp/cpp` | Walker 递归策略（分片感知/stego 分派/免 spool 快路径）+ Session 聚合根 + resolve_runtime_options |
| 壳 | `gui.hpp/cpp` | 密码/前缀弹窗（内存 DLGTEMPLATE）、进度窗、完成通知 |
| | `report.hpp/cpp` | 报告快照/渲染分离（纯函数可单测） |
| | `menu.hpp/cpp` | HKCU 右键级联菜单装卸 |
| | `main.cpp` | CLI 入口（双模式 exe：/SUBSYSTEM:WINDOWS + mainCRTStartup） |
| | `nxshell.cpp` | Win11 新版菜单 IExplorerCommand COM DLL（独立构建目标，稀疏 MSIX 用） |
| | `fuzz_main.cpp` | libFuzzer 全管线目标（CMake `NX_FUZZ`，禁 /GL） |

## 领域模型速览（详案见设计文档 §3/§4）

- **格式三分法驱动一切路由**：`FormatClass` = Filter（单流→单流）/ SeqContainer（tar/cpio/ar，天然流式）/ TailContainer（zip，尾部依赖）/ RandContainer（7z/rar/iso/cab/wim，需 seek）。`classify()` 的结果决定走 FilterStage 还是 ArchiveStage、是否经 SpoolStore。
- **核心抽象**：ByteSource（唯一流）→ PushbackSource（嗅探/重启的回看适配）→ SpoolStore（R 类 seek 适配）→ ContainerReader+EntryToken（条目迭代契约）→ Walker（递归策略 + LayerCtx 帧语义）→ Sink（安全落盘）。密码按 LayerId（逻辑路径键）每层独立解析。
- **嵌套容器免 spool 直读**：父视图 seekable 且条目 stored 时，子容器直接在父区间随机访问（RegionView 可链式套窗口）；任何失败自动回退 spool 原路径——**回退语义是硬边界**。
- **隐写检测**（仅根 FS 层，需 seek）：MP4 atom 步进（非法头即候选起点；7z/rar 尾部无结束标记只能经此发现）+ EOCD 反向扫描（CD 位置须 `PK\x01\x02` 自证——zip64 影子值场景）；假阳性由试开失败兜回未命中。
- **InputMeter 挂表纪律**：根输入直读视图挂表（进度分母/压缩比分母），码表探测视图与 spool 卷不挂（避免虚增）——经 ViewFactory 命名方法强制。

## CI 硬门（违例即不可交付）

`python tests/run_tests.py` 一把梭，**65 用例全绿是合并前提**；缺工具直接 FAIL（决策 D-2：门就是门）。构成：

| 门 | 内容 | 缺工具时 |
|---|---|---|
| unit_core | nxunit 325 项断言（纯核心：detect/sanitize/volumeset/namecodec/outcome/report/stego…） | 构建失败即 FAIL |
| 属性测试 | 44 用例：生成语料端到端解压 ≡ 逐层手工解压（全树哈希对比） | — |
| ownership_audit | ①圈禁 grep 门（五名单出 res/ 即 FAIL，每次运行先正/负样本自检）；②AST 强闭包检查器（clang-cl ast-dump → F\* 验证 + KaRaMeL 抽取的 closure_check.exe，校准基线见 `--calibrate`） | FAIL |
| TLA+ 双模型 | `tools/ownership.tla`（legacy 复现 replayQ_ 反例 / weakOnly 反例 / fixed 零违例）+ `tools/boundedqueue.tla`（closeNoWake/abandonNoWake 必违 / fixed 零违例零死锁） | 缺 java 直接 FAIL（tla2tools.jar 已 LFS 冻结入仓随检出，无取数步骤） |
| BoundedQueue 协议门 | abandon 后两侧必不阻塞等协议断言 | — |
| tidy_check | clang-tidy 四检查零警告（owning-memory/dangling-handle/mt-unsafe/unnecessary-value-param） | FAIL |
| GUI 冒烟 | 9 用例（窗口消息自动化，含取消中止/半成品清理） | — |
| context_menu | 真实装卸 HKCU 菜单（现场保存/还原） | — |

**发布前另跑**：`python tests/release_gate.py`（合成语料端到端哈希比对，manifest 入库，`--update` 固化基线；语料由 `gen_release_corpus.py` 确定性重建）。

### CI（GitHub Actions）

`.github/workflows/release.yml`：**推送 `v*` 标签触发**，在 CI 上跑**与本地相同的门体系**（run_tests 全量 + release_gate）后产出 `nx-<tag>-windows-x64.zip` 并创建 GitHub Release；手动 `workflow_dispatch` 可试跑（只出 artifact 不发布）。要点：

- **CI 自装工具链**：Java+`tla2tools.jar`（TLA+ 门）、LLVM（clang-cl/clang-tidy——VS Clang 组件缺席时 choco 兜底；`audit_ownership.py`/`tidy_check.py`/`build_closure_kernel.cmd` 均为可移植发现：`NX_VSROOT` → 本机默认 → vswhere → 独立 LLVM）、闭包内核（`tools/proofs/krml_runtime/` 收编 KaRaMeL 运行时，无需本地 F*）。
- **rar 语料 = LFS 冻结入库**（`tests/cases-rar/`，`freeze_rar_corpus.py` 产物 + SHA256SUMS 清单）：rar 只能由 WinRAR 试用版生成（专有），CI 把冻结目录拷入 `tests/cases/` 后照跑全部 rar 用例（读取引擎 7z.dll/libarchive 与生成工具无关）。本地有 WinRAR 时 `gen_corpus_m1.py` 实时重生成，冻结目录不受影响；`--force` 重冻需本地 WinRAR。
- **libarchive 7z/rar 回退覆盖**：`NX_NO_7ZDLL=1` 测试钩子（szcom `dll_available`）强制视为无 7z.dll——`rar5_plain_la`/`rar_solid_la` 用例走 libarchive 读取；加密（rar5 crypto 有限）与分卷（libarchive 无多卷）仍依赖 7z.dll，不在此列。
- 唯一残余缺口：rar4/旧命名卷——WinRAR 7.x 已不支持 `-ma4`，无语料生成渠道（README 已知限制 #3）。
- 便携包不含 nxshell.dll/menupkg（Win11 新版菜单雏形未启用；在用 = 经典级联菜单）。
- 发一个版本：本地全量门跑绿（含 WinRAR 语料）→ `git tag v0.x.y && git push origin v0.x.y`。

### 改动 → 必跑矩阵

| 改了什么 | 必跑 |
|---|---|
| 任何 `src/` 代码 | `build.cmd` + `run_tests.py` 全量 |
| walker/sink/password/detect 等敏感区 | 先读设计文档 §4.1/§9.6，再改；全量门 + fuzz 短跑 |
| 所有权/生命周期（esft 类、EntrySource、SpoolStore、队列协议） | 三层验证全过（TLA+ / AST / F\*）+ fuzz 长跑 |
| `src/res/` 或任何资源接入 | audit 门（run_tests 内）+ `build-diag.cmd` 构建跑加密样本（哨兵必须静默） |
| 过滤器泵/readEntryDirect/writeOne 等热路径 | bench 对比（`tests/bench.py`）+ fuzz，性能不得回退 |
| 引擎打开/回退逻辑 | `pw_retry_nested` 语料 + 加密样本 + fuzz（哨兵覆盖弃置路径） |
| 语料/测试本身 | 对应 gen_corpus 系列 + 受影响用例 |

退出码契约（属性测试断言依赖）：`0` 成功｜`1` 部分失败｜`2` 密码｜`3` 超限｜`4` 缺分片｜`64` 用法错误。
密码交互测试依赖 `NX_PROMPT_TEST=1`；`NX_EXE` 可覆盖被测 exe。

## 修改红线（摘要）

行为硬边界与性能红线、所有权纪律 P1-P6、安全纪律、决策速查（D-1~D-4）、踩坑清单——**全部在 [AGENTS.md](AGENTS.md)**，改代码前对照。一句话版本：不经显式决策不变更回退语义/密码链/熔断/退出码/原子落名；热路径不加间接层；资源只经 res/；密码绝不落日志；进度与计划写 `plan/` 不写仓库。
