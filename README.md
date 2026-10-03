# nx — 流式嵌套压缩包解压工具

设计文档：[nested-extractor-design.md](nested-extractor-design.md)（v0.3：设计权威 + §4.1 所有权与生命周期 + §9.6 验证体系）。
工作区纪律：[AGENTS.md](AGENTS.md)（所有权纪律 P1-P6 操作化 + 决策速查）。
**当前状态：M0–M3 + v1 后续 + 重构批次 0–6（全部）**。55/55 测试（unit_core 325 项 + 所有权双门 + BoundedQueue 协议门 + clang-tidy 基线门 + 50 属性）+ GUI 冒烟 9/9 + 合成发布门通过。C++23。

## 构建（Windows + VS 2026 + vcpkg）

```cmd
build.cmd       # 编译 → build\nx.exe（约 5.5 MB 单文件，仅系统 DLL 依赖）
package.cmd     # 便携打包 → dist\nx\（nx.exe + nxshell.dll + 7z.dll + menupkg + 文档）
```

依赖（vcpkg manifest 固定）：libarchive 3.8.7（容器）、**zlib-ng[compat]**/bzip2/liblzma/zstd/lz4（zlib-ng=SSE4/AVX2 inflate + PCLMUL CRC，compat 供三方经 <zlib.h> 链接）。
`ports-overlay/libarchive`：①上游 CMake 未链 crypto 探测 `PKCS5_PBKDF2_HMAC_SHA1` 导致 WinZip AES stub——强制定义修复；②`nx-batch-ctr.patch`——上游 WinZip AES 每 16 字节一次单块 EVP（实测 ~60MB/s），批量化为每 64KiB 一次（AES 2GiB 实测 33.6s→2.7s，内容校验一致；曾按上游风格提交 PR libarchive#3443，上游暂无 review 带宽已礼貌关闭，overlay 持续生效）。
`ports-overlay/zlib-ng`：基线端口无 feature，自建 `compat` feature（ZLIB_COMPAT 构建导出标准 ZLIB 配置）。

**7z.dll**（运行时按需加载）：exe 目录 → `C:\Program Files\7-Zip\` → PATH。
负责 7z 全特性（AES+头加密+分卷）与 RAR 解码；缺失时回退 libarchive（RAR 多卷除外）。

## 已实现（M0–M3 全量）

### M0 — 核心流水线
- ByteSource 抽象（File/Concat/Queue/Pushback 回看流）
- 有界队列背压 + 过滤器泵线程（zlib/bzip2/xz/zstd/lz4 直连，多成员串联）
- libarchive 容器引擎：密码候选迭代、D2 spool+中央目录回退
- 分片：`.001` 拼接 / `.z01+.zip` 顺序陷阱 / 条目级分组
- 分层密码链（缓存→上次成功→候选→交互/GUI），SecureStr 安全擦除
- Sink：路径消毒 / 大小写重名 / `.part` 原子落名 / 深度/总量/磁盘水位/压缩比熔断

### M1 — zip / 7z / rar 全格式
- `szcom.*`：7z.dll（IInArchive COM）适配层——CreateObject 入口、IInStream（文件/spool 卷窗口）、RAR 多卷卷回调（FS 直读 + spool 窗口）、双通道密码回调（打开/抽取）
- 7z：AES 内容加密 + 头加密（-mhe）+ `.7z.001` 拼接分卷
- rar：rar5 / solid / `-hp` 加密 / 新式多卷（.partN.rar）/ 条目级多卷（zip 内嵌分卷）
- zip：SFX 魔数扫描 + D2 回退；zip 中央目录模式（File/Spool SeekView）+ 码表探测（CP932 等 iconv 格式名）
- 招牌：zip→7z(密码A)→rar(密码B) 三格式异密码嵌套链

### M2 — 并发/背压调优、安全完备、--verify/--report、基准
- PushbackSource 游标化 + 检测后关闭历史（直通模式）+ `read_direct` 零拷贝链
- FS 级 7z/rar/zip 根文件免 spool 直读；7z.dll 惰性加载；Sink 写出线程池
- 压缩比熔断（分母含根尺寸提示，小输入炸弹也能判定）
- `--verify sha256`（BCrypt）+ `--report r.json`（不含密码）
- 基准（`python tests/bench.py`）：链式用例 nx 快于手工两遍 33-34%、峰值中间磁盘 0 MiB

### M3 — 便携分发、右键菜单、GUI 弹窗、默认日志
- 便携打包 `package.cmd`；右键菜单 `nx menu install|remove`（HKCU ExtendedSubCommandsKey 级联：extract-here `--no-root` / extract-into 前缀弹窗默认=去扩展名 stem / extract-stego）
- GUI 密码弹窗（内存 DLGTEMPLATE）：无控制台或 `--gui` 时自动，每层一窗，取消→整体中止；Ctrl+A 子类化补齐 + 激活即聚焦
- GUI 进度窗：独立线程无模式对话框，真百分比 = 根输入消耗比（InputMeter 挂表纪律：根 zip FileSeekView 与 7z.dll 直读挂、探测视图与 spool 卷不挂），取消→exit 2 静默，`.part` 照常清理
- 双模式 exe（`/SUBSYSTEM:WINDOWS`）：资源管理器启动无黑框，终端/管道行为不变
- 默认日志 `nx.log`（append，超 5 MiB 截断，密码红线过滤）；Win11 新版菜单 `menupkg/` 雏形未启用

### 隐写解压（extract-stego / --stego）
- 检测两条路（`stego.cpp`，仅根文件层）：MP4 atom 步进（非法原子头处即候选起点；mdat size=0 延伸到 EOF）+ EOCD 反向扫描（注释长度精确吃到 EOF；不要求 EOCD 在 EOF——伪装尾按区间排除，**CD 位置须 PK\x01\x02 自证**，不可信则回退魔数锚点窗口由 libarchive 依 EOCD64 真值定位——案例 L 的 zip64 影子场景）
- 打开：尾接 zip 走 FileSeekView 精确窗口；7z/rar 走 fsBase 窗口交 7z.dll
- 语义=只解隐写压缩包，根文件本体不落盘；未命中 exit 0 + 提示；EOCD 假阳性由试开失败兜回
- 8 属性用例 + GUI 冒烟动词端到端

### 嵌套容器免 spool 窗口直读
- 父视图 seekable 且条目 **stored** 时，嵌套 zip/7z/rar 直接在父区间随机访问，免全量 spool 往返（300MB stored 嵌套实测 0.14s；RegionView 可链式套窗口）
- 机制：数据相位的 read+seek 双记（libarchive 256KB read-ahead 命中时 read 回调不触发）→ 回溯 512KB 定位本地头（PK+stored+未加密+区间精确覆盖）→ 区间直读
- 安全网：任何失败（deflate 父条目/加密/推导误判/子打开失败）**自动回退 spool 原路径**

### 性能（v1 后续四项）
- zlib-ng[compat]（自建 overlay feature）：inflate/CRC SIMD 化
- spool RAM 自适应：默认空闲物理内存 50%（64MiB–8GiB，`--spool-ram` 覆盖）；溢出临时目录默认=输出目录（同盘零跨盘 I/O，`FILE_FLAG_TEMPORARY`）；临时文件 `DELETE_ON_CLOSE`
- WinZip AES 批量 CTR（overlay 补丁，AES 路径 ~12×）
- nx Release LTO（/GL /Gy /Oi + /LTCG）；bench A -34% 反超 bsdtar / B -47% / C -9%

### 稳定性修复（真实语料案例，已匿名化——结构见 tests/gen_release_corpus.py）
- **案例 L（15GB 隐写 MP4，三层嵌套加密）**：①输出路径 251 字符 + `.nxpart-` 后缀 >260，裸路径报 ERROR_PATH_NOT_FOUND(3) 而非"路径过长"——Sink 三处统一 `win_long_path()`；②成功运行后 14.86GB `nx-{GUID}.tmp` 残留——对象级泄漏（replayQ_ 环，见重构批次 0 D6）+ 强杀时 dtor 不执行——临时文件改 `FILE_FLAG_DELETE_ON_CLOSE`（句柄一关内核即删）
- **案例 XJ（隐写 MP4 → 7z SFX solid+AES）**：逐条目单独 `Extract` = 每文件从 solid 块头重解码（O(N²)，外推 8~12h）→ `materializeBatch` 批量抽取（一次 Extract 一批连续索引分发到各条目 spool；预算钳 [64MiB,1GiB]；坏点隔离）。实测 38s/3747 文件，与 7z CLI 哈希零差异；另补 `mx_` 串行化（7z.dll 单线程约定）
- **案例 M（11.23GiB 隐写 MP4）**：spool 整段落盘 cast DWORD 把 8GiB 截断成 0 → WriteFile 长度 0"成功"——分块 ≤16MiB 落盘
- **案例 Z/X（extract-into 撞名）**：默认前缀曾=完整文件名撞输入；ALREADY_EXISTS 未验证目录属性。修复：前缀=去扩展名 stem + 属性验证 + 失败按成因分类 + GUI 交互流弹窗告知（案例 X 的无扩展名 stem 回退残余场景）
- **案例 N（CP932 zip）**：C locale → libarchive NULL pathname（主因）。修复：`setlocale(LC_ALL, ".UTF8")` + 中央目录模式 + iconv 格式码表名 + 空名防御

### Fuzz 安全护城河
- 目标=全管线端到端（`src/fuzz_main.cpp`）：每迭代真实递归（detect/stego/引擎/密码链/Walker/Sink），覆盖面=生产路径本身；MSVC libFuzzer + ASan（独立构建 build-fuzz/）；泄漏哨兵 S1-S5 常开（泄漏=abort）
- 限额收紧保证单迭代有界：深度 3 / 输出 2MiB / 压缩比 50 / spool RAM 1MiB（促发磁盘溢出分支）
- `python tests/fuzz_run.py`（--time/--jobs/--rerun）；种子=tests/cases 全量 + `tests/fuzz-regression/`（git 跟踪）历史崩溃工件回灌——修复后的回归种子持久层
- 实绩：711 次迭代即抓到开发期 COM 释放顺序 use-after-free（工件已入回归种子）

## 重构记录（批次 0–6，2026-10-02/03 全部完成）

逐批交付如下（逐 commit 细节见 git 历史；所有权模型与生命周期状态机 → 设计文档 §4.1，
验证体系全景 → 设计文档 §9.6，操作化纪律 → AGENTS.md）。

| 批次 | 交付 |
|---|---|
| 0 | 缺陷登记簿 D1-D8 + 泄漏哨兵 S1-S5（fuzz 常开）。含 replayQ_ 自引用环结构性修复（重放队列只存元数据）与过滤器链深度约束（决策 D-1） |
| 1 | filter RAII 化 + pump_members 模板合并、pullBlock 归一、纯函数抽离（detect/stego/report/exit_code/sanitize/ascii_lower）、领域类型首批（kFormatTable/NameCodec/AccessRecorder/Detection.note）、C++23 + Result 别名、nxunit 单测壳 |
| 2 | LayerId key/display 拆分——密码缓存键=容器逻辑路径，修复兄弟分片组共享游标 → 假性 PasswordExhausted 真 bug；LayerCtx 收敛散参数 + Walker 类 |
| 3 | MemorySource 管线免文件系统测试、PromptSink 脚本化密码链（全语义单测）、select_group 三层单测 |
| 4 | TLA+ 前置验收门（legacy/weakOnly/fixed 三变体）、weak_ptr 条目源 + keepAlive() 令牌（配套）、EntryToken 契约、views 唯一实现 + ViewFactory 挂表纪律类型化、engines 拆五件、szcom cache_ 共享预算 LRU |
| 5 | res/ 资源圈禁（UniqueHandle 三别名/TempFile/com_ptr/DeleteGuard/gsl::owner，五名单 grep 硬门）；AST 强闭包检查器（clang-cl → F\* 验证 closure_check.exe，校准 f647037 恰报 LaSeqReader 零误报）；M1 短写修复。三路评审修复：UniqueFile 哨兵 nullptr 化（INVALID_HANDLE_VALUE 非 NTTP 合法常量）、audit fail-loud、fuzz 回归种子真正入库 |
| 6 | clang-tidy 基线硬门（四检查零警告）、/analyze 排雷零警告、合成发布语料 + release_gate 哈希门（决策 D-4：真实样本不入仓，按结构重建）、AGENTS 所有权纪律节、文档匿名化（案例代号）、BoundedQueue abandon 协议 TLA+ 模型（DeadRelease/ParkedSanity + 双校准反例） |

**仍生效的两条 RAII 化契约教训**：①DeleteGuard 声明序=先关句柄后删文件（反序被 GUI 用例 6 抓住 .part 残留）；②COM 释放顺序——Open 失败后 7z.dll 仍持流引用，`arc.reset()` 必须先于 `mainStream_.reset()`。

```bash
python tests/gen_corpus.py       # 基础语料（含隐写 9 组 + 嵌套直读 1 组；tests/cases、tests/work 均在 .gitignore）
python tests/gen_corpus_m1.py    # M1 语料（zip/7z/rar；需 tests/tools/winrar/Rar.exe + 7z CLI）
python tests/gen_corpus_m2.py    # M2 语料（压缩比炸弹）
python tests/gen_corpus_fn.py    # 文件名编码语料（CP932/GBK）
python tests/run_tests.py        # 55/55（unit_core 325 项 + 所有权双门 + BoundedQueue 协议门 + clang-tidy 基线门 + 50 属性）；NX_EXE 可覆盖被测 exe
python tests/release_gate.py     # 发布门：合成语料端到端哈希比对；--update 固化基线
python tests/gen_release_corpus.py  # 发布语料生成（确定性种子，缺则 release_gate 自动重建）
python tests/fuzz_run.py         # libFuzzer+ASan 全管线 fuzz（哨兵 S1-S5 常开）
python tests/bench.py            # 基准（3 语料 × 3 方案）
python tests/gui_smoke.py        # GUI 冒烟 9 用例（窗口消息自动化）
cmd /c build-analyze.cmd         # MSVC /analyze 排雷（低噪子集，非门；项目源零警告）
```

## 已知限制

| # | 问题 | 说明 | 优先级 |
|---|---|---|---|
| 1 | 嵌套隐写检测 | 隐写扫描仅根文件层（需 seek）；压缩包内的 MP4 不查（真实场景是右键单个文件） | 低 |
| 2 | 条目级分片连续到达 | 分片组成员须连续到达，非成员条目到达即封组 | 低 |
| 3 | RAR4/旧命名卷 | WinRAR 7.x 无法生成 rar4 语料（读取由 7z.dll 覆盖，无测试验证） | 低 |
| 4 | tar 内符号链接 | v1 降级策略——跳过并告警，不落盘 | 低 |
| 5 | Win11 新版右键菜单 | menupkg/ 稀疏 MSIX 雏形未启用（经典级联菜单完整可用） | 低 |
