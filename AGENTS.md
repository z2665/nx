# AGENTS.md — nx 工作区须知（AI 协作代理必读）

`nx`：Windows 专属的流式嵌套压缩包解压器（C++23，单 exe `build\nx.exe`）。

## 必读文档与工作目录（会话开始先做）

按序阅读，再动手：

1. **[nested-extractor-design.md](nested-extractor-design.md)** — 权威设计文档。改 walker/sink/password/detect 等敏感区域前必读；§4.1 所有权与生命周期、§9.6 验证体系。
2. **[DEVELOP.md](DEVELOP.md)** — 开发指南：构建、源码地图（文件→模块→职责）、领域模型速览、CI 硬门清单与红线。
3. **`plan/`** — **所有进度与计划的唯一落点**（本地文件夹，已 gitignore，不入仓）：
   - 会话开始先读该文件夹，了解当前进度、未竟事项与既定计划；
   - 工作中产生的进度记录、计划、任务清单一律写回该文件夹（一事一文件，建议 `YYYY-MM-DD-主题.md`），**不写进仓库文档**——仓库文档只描述"系统是什么、怎么开发"，不描述"开发到哪了"；
   - [README.md](README.md) 面向最终用户（功能/安装/用法），与开发无关。

## 构建与打包

```cmd
build.cmd         # CMake+Ninja+VS 2026（vcvars64）+vcpkg → build\nx.exe
package.cmd       # 便携打包 → dist\nx\（需先 build.cmd；可选复制 7z.dll）
```

- vcpkg manifest 固定依赖：libarchive 3.8.7 + zlib-ng[compat]/bzip2/liblzma/zstd/lz4，triplet `x64-windows-static`。
- 必须的 overlay 有两个：`ports-overlay/zlib-ng`（基线端口无 feature，自建 compat）与
  `ports-overlay/libarchive`（crypto 探测修复 + `nx-batch-ctr.patch` WinZip AES 批量化 + 上游 round-trip 测试；补丁背景见 DEVELOP 依赖节）。
- `build.cmd` 硬编码了本机代理 `127.0.0.1:10808` 与 `VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community`——换机器需改。
- 7z.dll 运行时按需加载（exe 目录 → 系统目录/PATH → Program Files → Program Files (x86)，见 szcom try_load），负责 7z 全特性与 RAR；缺失回退 libarchive。
- spool RAM 默认 0=自动（空闲物理内存 50%，64MiB–8GiB，`--spool-ram` 覆盖）；溢出临时目录默认=输出目录（同盘零跨盘 I/O）。

## 测试（硬门，违例即不可交付）

```bash
python tests/gen_corpus.py       # 基础语料（含隐写 9 组 + 嵌套直读 2 组；tests/cases、tests/work 均在 .gitignore）
python tests/gen_corpus_m1.py    # 需 tests/tools/winrar/Rar.exe + 7z CLI（无 WinRAR 时 rar 用例由 LFS 冻结语料 tests/cases-rar/ 补入——freeze_rar_corpus.py 产物）
python tests/freeze_rar_corpus.py # 重冻结 rar 语料到 LFS 目录（需先本地生成；--force 覆盖）
python tests/gen_corpus_fn.py    # 文件名编码语料（CP932/GBK）
python tests/gen_corpus_m2.py    # 限额语料（ratio bomb；CI 亦跑，勿漏）
python tests/run_tests.py        # 硬门：65 用例全绿（构成与门的语义见 DEVELOP「CI 硬门」；缺语料/缺工具一律 FAIL 不缩水）；NX_EXE 环境变量可覆盖被测 exe 路径
python tests/release_gate.py     # 发布门：合成语料端到端哈希比对；--update 固化基线
python tests/fuzz_run.py        # libFuzzer+ASan 全管线 fuzz（独立构建 build-fuzz/，gitignore；泄漏哨兵 S1-S5 常开）
python tests/bench.py            # 基准；python tests/gui_smoke.py  # GUI 冒烟 9 用例
cmd /c build-analyze.cmd         # MSVC /analyze 排雷（低噪子集，非门；项目源零警告）
```

- 退出码契约（测试断言依赖）：`0` 成功｜`1` 部分失败｜`2` 密码｜`3` 超限｜`4` 缺分片｜`64` 用法错误。
- 密码交互测试依赖环境变量 `NX_PROMPT_TEST=1`。
- context_menu 用例会真实装卸 HKCU 菜单——已做现场保存/还原（测试后用原 exe 重装），不会再吃掉用户菜单。
- fuzz 目标（`src/fuzz_main.cpp` + CMake `NX_FUZZ`）复用全部管线源但**不能开 /GL**（与 sanitizer
  不兼容）；MSVC libFuzzer 提供驱动 main；动态 ASan DLL 由 post-build 复制到 exe 旁。改管线代码后
  长跑 `fuzz_run.py` 是安全回归手段（`--rerun` 复现工件）。

## 架构分层（src/ 一文件一阶段，勿跨层直达）

```
VolumeSet(分片) → ByteSource(唯一流抽象) → Detector(嗅探)
  → FilterStage(过滤器直连) / ArchiveStage(libarchive+SpoolStore)
  → Walker(递归+Limiter+PasswordProvider) → Sink(安全落盘)
```

- `src/res/` 是**资源圈禁区（P2）**：Win32 句柄/COM/临时文件的 RAII 唯一来源
  （UniqueFile/UniqueRegKey/UniqueModule/TempFile/com_ptr/DeleteGuard，header-only）。
  口径=五名单：`CloseHandle/DeleteFileW/RegCloseKey/FreeLibrary/->Release()` 不得出现在
  res/ 之外——`tests/audit_ownership.py` 的 grep 圈禁门是硬门（每次运行先跑正/负样本
  自检），新代码违例直接 FAIL。名单外资源（fclose/LocalFree/CoTaskMemFree/
  archive_read_free）显式豁免。文件句柄接入一律走
  `res::adopt_file(CreateFileW(...))`（INVALID_HANDLE_VALUE 归一——哨兵是 nullptr）。

- `ByteSource` 是唯一流抽象；R 类（需 seek 的）容器经 `SpoolStore`（RAM 环形自适应 → 磁盘溢出），
  **stored 嵌套条目例外**——可经 `RegionSource` 区间直读免 spool（见下）。
- 引擎分工：libarchive=容器；zlib-ng/bzip2/lzma/zstd/lz4=过滤器直连；`szcom.cpp`=7z.dll COM 适配（IInArchive、多卷回调、双通道密码）。容器打开统一走 engines.hpp 门面（open.cpp 组合根），勿直达 szcom/laseq。
- 嵌套免 spool 直读：`RegionSource`（bytesource.hpp）= 父支撑中连续区间；`ByteSource::seekRegion()`
  经 SharedView/PushbackSource 转发；推导失败/deflate 父条目/子打开失败一律自动回退 spool——改这些类时保持回退语义。
- 线程模型：`std::jthread` + `stop_token`，级间固定容量有界队列背压；不引入协程。

## 安全纪律（不可妥协）

- 密码绝不写日志/`--report`；`SecureStr` 安全擦除；每层密码独立解析链。
- Sink 必须走路径消毒 + `.part` 临时名原子 rename；深度/总量/磁盘水位/压缩比熔断不可绕过。

## 所有权纪律（P1-P6，改资源相关代码前对照）

**行为硬边界与性能红线（P 原则的前提）**：免 spool 直读回退语义、密码解析链顺序、
熔断、退出码契约（0/1/2/3/4/64）、`.part` 原子落名——不经显式决策不变更；
filter 五解码泵、readEntryDirect、PushbackSource、writeOne、SFX/EOCD 扫描体
不做纯度牺牲（不 ranges 化）；热路径零拷贝不加间接层。

1. **图无环（P1）**：esft 类的成员容器不得持有"条目源或含条目源的类型"
   （EntryToken 契约，`container.hpp`）；条目源对读取器只持 `weak_ptr`，异步写出的
   存活由 Sink 任务捕获 `keepAlive()` 令牌——**两者必须配套**（weakOnly 是 TLA+
   反例已证形态）。新增/改组合类型后跑 run_tests 的 `ownership_audit` 硬门
   （AST 强闭包含自身 = 类型级自引用环，replayQ_ 事故的永久免疫）。
2. **释放圈禁（P2）**：Win32 句柄/COM 引用/临时文件只经 `src/res/` 的 RAII 类型；
   文件句柄接入一律 `res::adopt_file(CreateFileW(...))`（哨兵 nullptr，
   INVALID_HANDLE_VALUE 由它归一）。五名单出 res/ 即 audit FAIL（见架构分层节）。
   COM out 参数的引用计数移交用 com_ptr + NOLINT 附理由。
3. **泄漏可观测（P3）**：新资源类型若可能泄漏，按 `diag.hpp` S1-S5 模式加哨兵
   （fuzz 构建常开）；fuzz 崩溃工件修复后移入 `tests/fuzz-regression/` 登记——
   那是唯一持久的回归种子层（fuzz-corpus-seeds 会被每次运行重建）。
4. **RAII 优先（P4）**：新代码不得出现裸 new/裸句柄/手工清理；`clang-tidy` 基线门
   （run_tests `tidy_check` 用例，零警告）会拦 owning 裸指针与值拷贝浪费——
   真修优先，语义边界（sink 参数/jthread stop_token/COM 移交/CRT 豁免）才 NOLINT。
5. **纯核心/效果壳（P5）**：解析、推导、打分、消毒、决策写成纯函数进 nxunit；
   IO/日志/GUI/线程留在壳层。
6. **不变式显式化（P6）**：生命周期不变式落成注释契约或断言——如 sink writeOne
   的 DeleteGuard 声明序（先关句柄后删文件）、szcom tryOpen 的 COM 释放顺序
   （`arc.reset()` 先于 `mainStream_.reset()`）——不留在口头。

三层验证分工：TLA+（`tools/ownership.tla` + `boundedqueue.tla`）管设计语义、
AST 检查器管代码现状、F\* 证明（`tools/proofs/`）管检查器算法本身——改所有权
模型时三层都要过。

**决策速查**（详案见 git 历史）：D-1 递归深度默认 10、过滤器链纳入 `--depth`
（不直接计入 depth——保 tar.gz 根 noRoot 语义）；D-2 形式化工具只用成熟件
（TLA+/TLC + F\*），门就是门、缺工具直接 FAIL；D-3 圈禁=五名单，CRT/内存
分配器族（fclose/LocalFree/CoTaskMemFree/archive_read_free）显式豁免，语义
边界 NOLINT 附理由；D-4 真实样本/路径/密码不入仓——测试语料一律按结构自建。

## 踩过的坑（改动相关代码前先看 git log）

- **esft 类不得把 `shared_from_this()` 交给"将被自己持有的结构"**——LaSeqReader 的
  replayQ_ 自引用环（失败尝试的读取器永不析构 → spool/视图连带泄漏，15GB 临时文件残留
  案例根因）。防护三重：重放队列只存元数据（ReplayRecord）、try_open 失败出口哨兵 S3
  （src/diag.hpp，fuzz 常开）、AST 强闭包检查器。
- `setlocale(LC_ALL, ".UTF8")` 是关键修复——C locale 下 libarchive 返回 NULL pathname。
- zip 文件名解码走中央目录模式（File/Spool SeekView）；码表候选名须为 iconv 格式（如 `CP932`）；
  EOCD 的 cdSize/cdOffset 是**小端**（MP4 atom 是大端）；zip64 影子值须经 CD 签名自证。
- 右键级联用 HKCU `ExtendedSubCommandsKey`；CommandStore 方案仅 HKLM 受支持（已回退）。
- exe 是双模式（`/SUBSYSTEM:WINDOWS` + `mainCRTStartup`）：资源管理器启动无黑框，终端/管道行为不变——改入口/子系统前理解这一点。
- 7z SFX 前缀魔数扫描仍仅前 4 MiB；尾部隐写（MP4/多合一）走独立 `stego.cpp`
  （atom 步进 + EOCD 反扫，仅根 FS 层，`extract-stego`/`--stego` 显式启用）。
- `EngineOptions.meter`（根 InputMeter）是进度百分比与压缩比分母的公共数据源：
  根层直读视图（FileSeekView/FileSeekInput）挂、码表探测视图与 spool 卷不挂——动这些类时保持该纪律。
- **WriteFile/ReadFile 长度参数是 DWORD**：spool 8GiB 整段落盘 cast 截断成 0 曾报
  "写临时文件失败: 操作成功完成 (Win32 0)"——大块 I/O 一律分块（≤16MiB）。
- **libarchive read-ahead 缓冲（256KB）命中时 read 回调不触发**（小文件整包缓存）——
  依赖回调观察输入位置的逻辑须 seek+read 双记 + 主动促发读（嵌套直读的区间推导即此）。
- `ensure_dir_recursive` 对 ERROR_ALREADY_EXISTS 必须验证 FILE_ATTRIBUTE_DIRECTORY
  （同名文件占位会误报成功）；extract-into 默认前缀=去扩展名 stem（WinRAR 惯例，避开与输入文件同名）。

## 约定

- Windows 10+ only；MSVC `/W4 /permissive- /utf-8`（Release 另有 /GL /Gy /Oi + /LTCG）；长路径统一 `\\?\` 前缀。
- 文档与 commit message 用中文。
- 根目录 `probe*.obj`、`_vcpkg_run.cmd` 等为临时探针文件（*.obj 已 gitignore），勿提交；`tmp/` 为本地工作区（已 gitignore）。
