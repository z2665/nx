# AGENTS.md — nx 工作区须知

`nx`：Windows 专属的流式嵌套压缩包解压器（C++20，单 exe `build\nx.exe`）。
权威设计文档：[nested-extractor-design.md](nested-extractor-design.md)（改 walker/sink/password/detect 等敏感区域前必读）。
重构计划：[refactor-roadmap.md](refactor-roadmap.md)（批次 0-4 已完成，2026-10-02；剩批次 5 res/ 圈禁、批次 6 验证常态化）。
进度与已知问题以 [README.md](README.md) 为准（当前 M0–M3 + v1 后续全量 + 重构批次 0-4，52/52 测试通过；C++23）。

## 构建与打包

```cmd
build.cmd         # CMake+Ninja+VS 2026（vcvars64）+vcpkg → build\nx.exe
package.cmd       # 便携打包 → dist\nx\（需先 build.cmd；可选复制 7z.dll）
```

- vcpkg manifest 固定依赖：libarchive 3.8.7 + zlib-ng[compat]/bzip2/liblzma/zstd/lz4，triplet `x64-windows-static`。
- 必须的 overlay 有两个：`ports-overlay/zlib-ng`（基线端口无 feature，自建 compat）与
  `ports-overlay/libarchive`（crypto 探测修复 + `nx-batch-ctr.patch` WinZip AES 批量化 +
  `nx-batch-ctr-test.patch` 上游 round-trip 测试）。
- `build.cmd` 硬编码了本机代理 `127.0.0.1:10808` 与 `VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community`——换机器需改。
- 7z.dll 运行时按需加载（exe 目录 → Program Files → PATH），负责 7z 全特性与 RAR；缺失回退 libarchive。
- spool RAM 默认 0=自动（空闲物理内存 50%，64MiB–8GiB，`--spool-ram` 覆盖）；溢出临时目录默认=输出目录（同盘零跨盘 I/O）。

## 测试

```bash
python tests/gen_corpus.py       # 基础语料（含隐写 9 组 + 嵌套直读 1 组；tests/cases、tests/work 均在 .gitignore）
python tests/gen_corpus_m1.py    # 需 tests/tools/winrar/Rar.exe + 7z CLI
python tests/gen_corpus_fn.py    # 文件名编码语料（CP932/GBK）
python tests/run_tests.py        # 测试 51/51（unit_core 纯函数单测 + 50 属性）；NX_EXE 环境变量可覆盖被测 exe 路径
python tests/fuzz_run.py        # libFuzzer+ASan 全管线 fuzz（独立构建 build-fuzz/，gitignore；泄漏哨兵 S1-S5 常开）
python tests/bench.py            # 基准；python tests/gui_smoke.py  # GUI 冒烟 9 用例
```

- 退出码契约（测试断言依赖）：`0` 成功｜`1` 部分失败｜`2` 密码｜`3` 超限｜`4` 缺分片。
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

- `ByteSource` 是唯一流抽象；R 类（需 seek 的）容器经 `SpoolStore`（RAM 环形自适应 → 磁盘溢出），
  **stored 嵌套条目例外**——可经 `RegionSource` 区间直读免 spool（见下）。
- 引擎分工：libarchive=容器；zlib-ng/bzip2/lzma/zstd/lz4=过滤器直连；`szcom.cpp`=7z.dll COM 适配（IInArchive、多卷回调、双通道密码）。
- 嵌套免 spool 直读：`RegionSource`（bytesource.hpp）= 父支撑中连续区间；`ByteSource::seekRegion()`
  经 SharedView/PushbackSource 转发；推导失败/deflate 父条目/子打开失败一律自动回退 spool——改这些类时保持回退语义。
- 线程模型：`std::jthread` + `stop_token`，级间固定容量有界队列背压；不引入协程。

## 安全纪律（不可妥协）

- 密码绝不写日志/`--report`；`SecureStr` 安全擦除；每层密码独立解析链。
- Sink 必须走路径消毒 + `.part` 临时名原子 rename；深度/总量/磁盘水位/压缩比熔断不可绕过。

## 踩过的坑（改动相关代码前先看 git log）

- **esft 类不得把 `shared_from_this()` 交给"将被自己持有的结构"**——LaSeqReader 的
  replayQ_ 自引用环（失败尝试的读取器永不析构 → spool/视图连带泄漏，15GB 临时文件残留
  案例根因）。防护三重：重放队列只存元数据（ReplayRecord）、try_open 失败出口哨兵 S3
  （src/diag.hpp，fuzz 常开）、批次 5 的 AST 强闭包检查器。
- `setlocale(LC_ALL, ".UTF8")` 是关键修复——C locale 下 libarchive 返回 NULL pathname（D:\…\2.zip 案例）。
- zip 文件名解码走中央目录模式（File/Spool SeekView）；码表候选名须为 iconv 格式（如 `CP932`）；
  EOCD 的 cdSize/cdOffset 是**小端**（MP4 atom 是大端）；zip64 影子值须经 CD 签名自证。
- 右键级联用 HKCU `ExtendedSubCommandsKey`；CommandStore 方案仅 HKLM 受支持（已回退）。
- exe 是双模式（`/SUBSYSTEM:WINDOWS` + `mainCRTStartup`）：资源管理器启动无黑框，终端/管道行为不变——改入口/子系统前理解这一点。
- 7z SFX 前缀魔数扫描仍仅前 4 MiB；尾部隐写（MP4/多合一）走独立 `stego.cpp`
  （atom 步进 + EOCD 反扫，仅根 FS 层，`extract-stego`/`--stego` 显式启用）。
- `EngineOptions.meter`（根 InputMeter）是进度百分比与压缩比分母的公共数据源：
  根层直读视图（FileSeekView/FileSeekInput）挂、码表探测视图与 spool 卷不挂——动这些类时保持该纪律。
- **WriteFile/ReadFile 长度参数是 DWORD**：spool 8GiB 整段落盘 cast 截断成 0 曾报
  "写临时文件失败: 操作成功完成 (Win32 0)"（11.23GiB 案例）——大块 I/O 一律分块（≤16MiB）。
- **libarchive read-ahead 缓冲（256KB）命中时 read 回调不触发**（小文件整包缓存）——
  依赖回调观察输入位置的逻辑须 seek+read 双记 + 主动促发读（嵌套直读的区间推导即此）。
- `ensure_dir_recursive` 对 ERROR_ALREADY_EXISTS 必须验证 FILE_ATTRIBUTE_DIRECTORY
  （同名文件占位会误报成功）；extract-into 默认前缀=去扩展名 stem（WinRAR 惯例，避开与输入文件同名）。

## 约定

- Windows 10+ only；MSVC `/W4 /permissive- /utf-8`（Release 另有 /GL /Gy /Oi + /LTCG）；长路径统一 `\\?\` 前缀。
- 文档与 commit message 用中文。
- 根目录 `probe*.obj`、`_vcpkg_run.cmd` 等为临时探针文件（*.obj 已 gitignore），勿提交；`tmp/` 为本地工作区（已 gitignore）。
