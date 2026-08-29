# AGENTS.md — nx 工作区须知

`nx`：Windows 专属的流式嵌套压缩包解压器（C++20，单 exe `build\nx.exe`）。
权威设计文档：[nested-extractor-design.md](nested-extractor-design.md)（改 walker/sink/password/detect 等敏感区域前必读）。
进度与已知问题以 [README.md](README.md) 为准（当前 M0–M3 完成，34/34 测试通过）。

## 构建与打包

```cmd
build.cmd         # CMake+Ninja+VS 2026（vcvars64）+vcpkg → build\nx.exe
package.cmd       # 便携打包 → dist\nx\（需先 build.cmd；可选复制 7z.dll）
```

- vcpkg manifest 固定依赖：libarchive 3.8.7 + zlib/bzip2/liblzma/zstd/lz4，triplet `x64-windows-static`。
- `ports-overlay/libarchive` 是必须的 overlay（上游 CMake 漏链 crypto 探测导致 WinZip AES stub）。
- `build.cmd` 硬编码了本机代理 `127.0.0.1:10808` 与 `VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community`——换机器需改。
- 7z.dll 运行时按需加载（exe 目录 → Program Files → PATH），负责 7z 全特性与 RAR；缺失回退 libarchive。

## 测试

```bash
python tests/gen_corpus.py       # 先重建语料（tests/cases、tests/work 均在 .gitignore）
python tests/gen_corpus_m1.py    # 需 tests/tools/winrar/Rar.exe + 7z CLI
python tests/gen_corpus_fn.py    # 文件名编码语料（CP932/GBK）
python tests/run_tests.py        # 属性测试；NX_EXE 环境变量可覆盖被测 exe 路径
python tests/bench.py            # 基准；python tests/gui_smoke.py  # GUI 冒烟
```

- 退出码契约（测试断言依赖）：`0` 成功｜`1` 部分失败｜`2` 密码｜`3` 超限｜`4` 缺分片。
- 密码交互测试依赖环境变量 `NX_PROMPT_TEST=1`。

## 架构分层（src/ 一文件一阶段，勿跨层直达）

```
VolumeSet(分片) → ByteSource(唯一流抽象) → Detector(嗅探)
  → FilterStage(过滤器直连) / ArchiveStage(libarchive+SpoolStore)
  → Walker(递归+Limiter+PasswordProvider) → Sink(安全落盘)
```

- `ByteSource` 是唯一流抽象；只有 R 类（需 seek 的）容器经 `SpoolStore`（RAM 环形 → 磁盘溢出）。
- 引擎分工：libarchive=容器；zlib/bzip2/lzma/zstd/lz4=过滤器直连；`szcom.cpp`=7z.dll COM 适配（IInArchive、多卷回调、双通道密码）。
- 线程模型：`std::jthread` + `stop_token`，级间固定容量有界队列背压；不引入协程。

## 安全纪律（不可妥协）

- 密码绝不写日志/`--report`；`SecureStr` 安全擦除；每层密码独立解析链。
- Sink 必须走路径消毒 + `.part` 临时名原子 rename；深度/总量/磁盘水位/压缩比熔断不可绕过。

## 踩过的坑（改动相关代码前先看 git log）

- `setlocale(LC_ALL, ".UTF8")` 是关键修复——C locale 下 libarchive 返回 NULL pathname（D:\…\2.zip 案例）。
- zip 文件名解码走中央目录模式（File/Spool SeekView）；码表候选名须为 iconv 格式（如 `CP932`）。
- 右键级联用 HKCU `ExtendedSubCommandsKey`；CommandStore 方案仅 HKLM 受支持（已回退）。
- exe 是双模式（`/SUBSYSTEM:WINDOWS` + `mainCRTStartup`）：资源管理器启动无黑框，终端/管道行为不变——改入口/子系统前理解这一点。
- 7z SFX 魔数扫描仅前 4 MiB（MP4 尾部隐写识别是待办 #2）。

## 约定

- Windows 10+ only；MSVC `/W4 /permissive- /utf-8`；长路径统一 `\\?\` 前缀。
- 文档与 commit message 用中文。
- 根目录 `probe*.obj`、`_vcpkg_run.cmd` 等为临时探针文件（*.obj 已 gitignore），勿提交。
