# nx — 流式嵌套压缩包解压工具

设计文档：[nested-extractor-design.md](nested-extractor-design.md)（v0.2 + M0–M3 实施记录）。
**当前状态：M0–M3 完成 + 真实语料验证**。34/34 属性测试通过。

## 构建（Windows + VS 2026 + vcpkg）

```cmd
build.cmd       # 编译 → build\nx.exe（约 5.5 MB 单文件，仅系统 DLL 依赖）
package.cmd     # 便携打包 → dist\nx\（nx.exe + nxshell.dll + 7z.dll + menupkg + 文档）
```

依赖（vcpkg manifest 固定）：libarchive 3.8.7（容器）、zlib/bzip2/liblzma/zstd/lz4（过滤器直连）。
`ports-overlay/libarchive`：上游 CMake 未链 crypto 探测 `PKCS5_PBKDF2_HMAC_SHA1` 导致 WinZip AES stub——overlay 强制定义修复。

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
- `szcom.*`：7z.dll（IInArchive COM）适配层——CreateObject 入口、IInStream（文件/spool 卷窗口）、
  RAR 多卷卷回调（FS 直读 + spool 窗口）、双通道密码回调（打开/抽取）
- 7z：AES 内容加密 + 头加密（-mhe）+ `.7z.001` 拼接分卷
- rar：rar5 / solid / `-hp` 加密 / 新式多卷（.partN.rar）/ 条目级多卷（zip 内嵌分卷）
- zip：SFX 魔数扫描（D1）+ D2 回退
- 招牌：zip→7z(密码A)→rar(密码B) 三格式异密码嵌套链

### M2 — 并发/背压调优、安全完备、--verify/--report、基准
- PushbackSource 游标化 + 检测后关闭历史（直通模式）+ `read_direct` 零拷贝链
- FS 级 7z/rar/zip 根文件免 spool 直读；7z.dll 惰性加载；Sink 写出线程池
- 压缩比熔断（分母含根尺寸提示，小输入炸弹也能判定）
- `--verify sha256`（BCrypt）+ `--report r.json`（不含密码）
- 基准（`python tests/bench.py`）：链式用例 nx 快于手工两遍 33-34%、峰值中间磁盘 0 MiB

### M3 — 便携分发、右键菜单、GUI 弹窗、默认日志
- 便携打包：`package.cmd` → `dist\nx\`（免安装）
- 右键菜单：`nx menu install|remove`（HKCU ExtendedSubCommandsKey 级联，经典菜单可靠展开）
  - 解压到当前目录（`extract-here`，--no-root）
  - 解压到指定目录…（`extract-into`，GUI 前缀弹窗，默认=压缩文件名）
- GUI 密码弹窗（内存 DLGTEMPLATE）：无控制台或 `--gui` 时自动，每层一窗，取消→整体中止
- 双模式 exe（`/SUBSYSTEM:WINDOWS`）：资源管理器启动无黑框，终端/管道行为不变
- 默认日志 `nx.log`：运行头+全部输出+report JSON；append，超 5 MiB 截断
- Win11 新版右键菜单：`menupkg/` 留有 IExplorerCommand+稀疏 MSIX 方案雏形（nxshell.dll/清单/脚本），未启用

### GUI 进度窗（原待办 #1）+ 真百分比（原待办 #2）
- 独立 GUI 线程上的无模式进度对话框：输入文件名 + 当前活动（容器展开/过滤器解码/当前写出文件）
  + 进度条 + 已输出字节/文件数/耗时（200ms 定时轮询 `Stats` 原子量，免锁）
- **真百分比**：`meter.bytes / stats.inputTotal`（根输入消耗比）。
  `InputMeter*` 经 `EngineOptions` 透传：根 zip 的 `FileSeekView` 与 7z.dll 直读的
  `FileSeekInput`（FS 卷，含 RAR 多卷回调卷）已挂计量；码表探测视图与 spool 卷不挂
  （前者多候选重读中央目录会虚增，后者字节来自外层已计量流）。
  `inputTotal` 由 `run_input` 按单文件/分片组各卷大小累计。重读超出由 99% 封顶吸收，
  分母未知回退动画条。tar/gzip 流式根顺带升级为真百分比
- 显示条件与完成弹窗一致：Explorer/右键启动（无标准句柄）或 `--gui`，且非 `tree`
- 取消（按钮/X）→ `abortFlag` → Walker/Sink 抛 `Cancelled` → exit 2 静默退出；
  大文件写出循环内逐块响应，`.part` 半成品照常清理
- `gui_smoke.py` 扩至 7 用例（进度窗出现/自动关闭/取消中止/半成品清理/
  zip 直读与 7z 直读的百分比爬升——`PBM_GETPOS` 采样断言）

### 真实语料修复（D:\…\2.zip 案例）
- **根因三层**：C locale → libarchive NULL pathname（主因）／本地头 EUC-JP vs 中央目录 UTF-8 不一致 ／
  码表候选名须 iconv 格式
- 修复：`setlocale(LC_ALL, ".UTF8")` + zip 改中央目录模式（File/Spool SeekView）+ `CP932` 格式码表探测 +
  空名防御合成 `__noname_N`
- CP932/GBK 独立语料回归通过

## 测试

```bash
python tests/gen_corpus.py      # M0 语料（14 组）
python tests/gen_corpus_m1.py   # M1 语料（zip/7z/rar；需 tests/tools/winrar/Rar.exe + 7z CLI）
python tests/gen_corpus_m2.py   # M2 语料（压缩比炸弹）
python tests/gen_corpus_fn.py   # 文件名编码语料（CP932/GBK）
python tests/run_tests.py       # 34/34 属性测试
python tests/bench.py           # 基准（3 语料 × 3 方案）
python tests/gui_smoke.py       # GUI 冒烟（窗口消息自动化）
```

## 已知限制与待办

| # | 问题 | 说明 | 优先级 |
|---|---|---|---|
| 1 | **MP4 隐写压缩包识别** | 部分 MP4 文件尾部隐写有 zip/rar 压缩包（7-Zip `#` 模式可打开）。当前 SFX 魔数扫描仅前 4 MiB，MP4 的 mdat 原子可达数 GB，需要扩展 MP4 原子解析或全文件扫描 | 中 |
| 2 | 条目级分片连续到达 | M0 限制：分片组成员须连续到达，非成员条目到达即封组 | 低 |
| 3 | RAR4/旧命名卷 | WinRAR 7.x 无法生成 rar4 语料（读取由 7z.dll 覆盖，无测试验证） | 低 |
| 4 | tar 内符号链接 | v1 降级策略——跳过并告警，不落盘 | 低 |
| 5 | unRAR 插件 | 经评估略过（7z.dll 已覆盖 RAR 主线） | — |

（原待办 #1「GUI 无进度指示」与 #2「进度条真百分比」均已完成，见上「GUI 进度窗」小节。）
