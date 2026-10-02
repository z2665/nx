# nx — 流式嵌套压缩包解压工具

设计文档：[nested-extractor-design.md](nested-extractor-design.md)（v0.2 + M0–M3 实施记录 + v1 后续）。
重构计划：[refactor-roadmap.md](refactor-roadmap.md)（批次 0 缺陷登记簿 D1–D8 已全部修复，2026-10-02）。
**当前状态：M0–M3 完成 + 真实语料验证 + GUI 进度/隐写解压 + 性能优化/嵌套免 spool 直读 + 重构批次 0/1/2/3/4**。52/52 测试（unit_core 298 项 + 所有权模型门 + 50 属性）+ GUI 冒烟 9/9 通过。

## 构建（Windows + VS 2026 + vcpkg）

```cmd
build.cmd       # 编译 → build\nx.exe（约 5.5 MB 单文件，仅系统 DLL 依赖）
package.cmd     # 便携打包 → dist\nx\（nx.exe + nxshell.dll + 7z.dll + menupkg + 文档）
```

依赖（vcpkg manifest 固定）：libarchive 3.8.7（容器）、**zlib-ng[compat]**/bzip2/liblzma/zstd/lz4（zlib-ng=SSE4/AVX2 inflate + PCLMUL CRC，compat 供三方经 <zlib.h> 链接）。
`ports-overlay/libarchive`：①上游 CMake 未链 crypto 探测 `PKCS5_PBKDF2_HMAC_SHA1` 导致 WinZip AES stub——强制定义修复；②`nx-batch-ctr.patch`——上游 WinZip AES 每 16 字节一次单块 EVP（实测 ~60MB/s），批量化为每 64KiB 一次（AES 2GiB 实测 33.6s→2.7s，内容校验一致）。
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
  - 解压到指定目录…（`extract-into`，GUI 前缀弹窗，默认=去扩展名文件名——WinRAR 惯例，
    避免输出目录与输入文件同名）
  - 解压隐写压缩包…（`extract-stego`，GUI 前缀弹窗，默认=文件名去扩展名 + `_stego`）
- GUI 密码弹窗（内存 DLGTEMPLATE）：无控制台或 `--gui` 时自动，每层一窗，取消→整体中止；
  输入框支持 Ctrl+A 全选（Win32 EDIT 原生不支持，子类化补齐）+ 窗口激活即聚焦输入框
  （弹窗自解压工作线程创建，常拿不到前台焦点，用户点活窗口后焦点直落输入框）
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
- `gui_smoke.py` 共 9 用例（前缀默认值（去扩展名，同名冲突回归）/取消/密码/进度窗
  出现·自动关闭·百分比爬升·取消中止/隐写动词端到端/无扩展名输入撞名错误弹窗——窗口消息自动化 + `PBM_GETPOS` 采样）

### 隐写解压（原待办：MP4 隐写压缩包识别）
- **检测两条路**（`stego.cpp`，仅根文件层——需 seek 跳过 GB 级 mdat）：
  - MP4 atom 步进：逐原子头小读、按 size 跳越，非法头处即隐写候选起点
    （7z/rar 尾部无结束标记，只能经此发现）；mdat size=0 视为延伸到 EOF；
  - EOCD 反向扫描：文件尾 64 KiB+22 回扫 `PK\x05\x06`，注释长度须精确吃到 EOF——
    覆盖任意格式文件的尾接 zip（jpg+zip 多合一、mdat size=0 病态 MP4）。
  I/O 成本：几十次小读 + 尾部一块，GB 级文件毫秒级。
- **打开**：尾接 zip 走 `FileSeekView` 窗口（EOCD 精确区间排除尾部伪装）；7z/rar 走
  `FileSeekInput` 的 `fsBase` 窗口（`[offset, EOF)` 呈现为完整卷）交 7z.dll。
- **真实样本三重陷阱**（D:\…\1.mp4，2.5 GB，已解）：
  ① EOCD 之后拖 18 KB 伪装数据 + 末尾假 `mdat` 原子（防尾部回扫类检测）——EOCD 校验
  不要求精确到 EOF，窗口按区间排除尾巴；② zip 前有 76 字节诱饵偏移；③ **zip64 影子值**——
  真值在 EOCD64+定位器，经典 EOCD 的 cdOffset/cdSize/条目数全是错的，数学基址会偏——
  区间必须自证（算出的 CD 位置验 `PK\x01\x02` 签名），不信任则回退"魔数锚点+到 EOF"
  窗口，由 libarchive 依 EOCD64 真值定位。
- **入口**：右键第三项 `extract-stego`（交互复刻 extract-into）或 CLI `extract --stego`；
  语义=只解隐写压缩包，根文件本体不落盘；输出照常走 Walker 递归 + Sink 熔断/消毒/密码链。
- 未命中：exit 0 + CLI 消息 / GUI「nx 隐写解压」信息框；EOCD 假阳性由试开失败兜回未命中。
- 8 个属性用例（mp4+zip / jpg+zip / mdat0+zip / mp4+rar / mp4+7z 加密 / 干净 MP4 未命中 /
  伪装样本明文+加密 / zip64 影子 EOCD）+ GUI 冒烟用例 8（动词端到端、默认前缀 `<名>_stego`）。

### 性能（v1 后续四项）
- **zlib-ng[compat]**（自建 overlay feature）：inflate/CRC SIMD 化
- **spool RAM 自适应**：默认空闲物理内存 50%（64MiB–8GiB，`--spool-ram` 覆盖）；溢出临时目录默认=输出目录（同盘零跨盘 I/O，`FILE_ATTRIBUTE_TEMPORARY` 驻留系统缓存）
- **WinZip AES 批量 CTR**（libarchive overlay 补丁 `nx-batch-ctr.patch`）：单块 EVP→64KiB 批量，
  AES 路径 ~12×（2GiB 实测 33.6s→2.7s 内容校验一致）；曾按上游风格提交 PR libarchive#3443
  （含 round-trip 测试），上游暂无 review 带宽已礼貌关闭——overlay 补丁持续生效
- bench（256MB 语料）：A 0.67→0.44s（-34%，反超 bsdtar）· B 1.27→0.67s（-47%，恢复快于手工两遍）· C 3.26→2.97s（-9%）
- nx Release 开启 LTO（/GL /Gy /Oi + /LTCG；实测无感——热路径在依赖库）

### 嵌套容器免 spool 窗口直读（v1 后续）
- 父容器为 seekable 视图支撑且条目为 **stored** 时，嵌套 zip/7z/rar 直接在父区间上
  随机访问，**免全量 spool 往返**（实测 300MB stored 嵌套 0.14s；8GB 级嵌套省掉整轮磁盘搬运与内存占用）
- 机制：条目数据相位的视图访问记录（read+seek 双记——libarchive 256KB read-ahead 缓冲
  命中时 read 回调不触发）→ 从首读位置回溯 512KB 定位本地头（PK+stored+未加密+区间精确覆盖，
  zip 条目区间互不重叠保证唯一）→ `RegionView` 可链式套窗口（zip-in-zip-in-zip 逐层直读）
- 安全网：任何失败（deflate 父条目语义上无连续区间/加密/推导误判/子打开失败）**自动回退
  spool 原路径**，行为与基线一致
- 边界：deflate 方式打包嵌套档案的外层（不可压缩数据仍标 deflate 的工具产物）不适用——
  载荷必须解压，语义上无区间

### 稳定性修复（真实语料案例）
- **输出路径超 MAX_PATH（案例 L 15GB 隐写案例）**：最终路径 251 字符 + `.nxpart-`
  临时名后缀超 260，`Sink::writeOne` 的 `CreateFileW`/`MoveFileExW`/`DeleteFileW`
  用裸路径——超 260 的裸路径报 **ERROR_PATH_NOT_FOUND(3)** 而非"路径过长"，目录
  （`ensure_dir_recursive` 内部有 `\?`）全建成、偏偏文件全失败。修复：三处调用统一
  `win_long_path()`（与目录创建同一规范，支持 32767）。语料 long_path（rel 271 字符）
  回归（构建前同构造已复现同错）。真实验证：15GB stego → 32 文件 13.84GiB exit 0，
  此前失败的 267 字符深路径文件正常落盘
- **spool 溢出临时文件残留**（同案例附带发现）：成功运行结束后 14.86GB `nx-{GUID}.tmp`
  残留在输出目录——对象级泄漏（该链路上某 zip 读取器整体未析构，退出转储证实
  spool 存活、2 个强引用）+ 强杀进程时 dtor 不会执行。修复：临时文件创建改
  `FILE_FLAG_DELETE_ON_CLOSE`——句柄一关（正常/异常/被杀）内核即删，清理责任
  不再依赖对象生命周期；验证同场景残留 0。读取器泄漏本身留待后续（影响仅内存壳）
- **solid 7z 逐条目抽取 O(N²)**（案例 XJ 隐写案例）：内层 7z SFX
  为 solid（3692 文件 2.3GB，LZMA2+BCJ2+AES），szcom 逐条目单独 `Extract` = 每文件
  从 solid 块头重解码到目标位置，实测外推 8~12 小时。修复：**批量抽取**
  （`materializeBatch`：一次 `Extract` 携带一批连续索引，`GetStream` 按 index 分发到
  各条目 spool；预算 = spoolRam/2 钳 [64MiB,1GiB]；批失败丢弃整批仅单条重试，坏点
  隔离、其后条目自成新批——keepGoing 语义不变）。真实验证：38s / 3747 文件 / 2.53GiB，
  与 7z CLI 单遍结果共同条目哈希零差异（.save 嵌套 zip 按设计递归多解 66 文件）。
  另修正隐患：Sink 线程池并发 `readEntry` 对 `arc_->Extract`/`cache_` 无锁——
  补 `mx_` 整体串行（7z.dll IInArchive 单线程约定）。语料 7z_solid_many（600 文件
  solid+AES）+ 时间断言（<60s；逐条目回退分钟级即抓）
- **spool 溢出 4GiB DWORD 截断**（11.23GiB 隐写 MP4 案例）：`flushToTemp` 整段落盘
  `cast DWORD` 把 8GiB（=2×4GiB）截断成 0 → `WriteFile` 长度 0 成功返回 → 报错竟是
  "写临时文件失败: 操作成功完成 (Win32 0)"。修复：分块 ≤16MiB 落盘。M0 起潜伏，
  spool 自适应 8GiB 后首次暴露
- **extract-into 输出目录与输入文件同名**（案例 Z）：默认前缀曾=完整文件名 →
  与输入 zip 同名；`ensure_dir_recursive` 把 ALREADY_EXISTS 误判成功（同名文件占位），
  解到子条目才失败且错误仅在 stderr。修复：默认前缀改去扩展名 stem（WinRAR 惯例）+
  ALREADY_EXISTS 验证 FILE_ATTRIBUTE_DIRECTORY。
  **无扩展名残余场景**（`案例 X` 真实案例）：无小数点输入 stem 回退=完整文件名，
  撞名仍发生且 GUI 右键场景下仅 log_err 用户完全不可见（提前 return 1 绕过完成弹窗）。
  修复：创建失败按成因分类（撞输入文件/被同名文件占用/其他）+ **GUI 交互流弹窗告知**
  （extract-into/-stego 刚弹过前缀窗或 Explorer/`--gui` 启动时 `MessageBox` 指引换前缀；
  纯终端仍走 stderr 不打扰）

### Fuzz 安全护城河（v1 后续）
- **目标=全管线端到端**（`src/fuzz_main.cpp`）：每迭代输入写临时文件 → `run_input` 真实
  递归（detect/stego/容器引擎/密码链/Walker/Sink 消毒落盘），覆盖面=生产路径本身
- MSVC libFuzzer + ASan（独立构建目录 `build-fuzz/`，`-DNX_FUZZ=ON`；动态 ASan 运行时
  随构建复制到 exe 旁，脱离 VS 环境可跑）；vcpkg 依赖为非插桩静态库——本仓代码带
  覆盖率与内存检测，依赖库内硬崩溃仍被捕获
- 限额收紧保证单迭代成本有界：深度 3 / 输出 2MiB / 压缩比 50 / spool RAM 1MiB
  （促发磁盘溢出分支）；`stegoMode`/`noRoot` 由输入尺寸奇偶派生（libFuzzer 需确定性）
- 运行：`python tests/fuzz_run.py`（默认 10 分钟；`--time` / `--jobs N`（-fork 并行）/
  `--rerun <file>` 复现工件）；种子=tests/cases 全量（≤1MiB），累积语料跨次增长
- 进程内静音：`log_set_quiet`（含 walker 层级列表的纯控制台显示），不写 nx.log
- 首跑实测（5 分钟）：22,947 次 / 76 exec/s / 0 崩溃，峰值 RSS 457MB；自动字典已习得
  CP936/CP932（码表探测）、各格式魔数、`ftyp`（MP4 atom 步进）等深层特征——覆盖真实

### 真实语料修复（D:\…\2.zip 案例）
- **根因三层**：C locale → libarchive NULL pathname（主因）／本地头 EUC-JP vs 中央目录 UTF-8 不一致 ／
  码表候选名须 iconv 格式
- 修复：`setlocale(LC_ALL, ".UTF8")` + zip 改中央目录模式（File/Spool SeekView）+ `CP932` 格式码表探测 +
  空名防御合成 `__noname_N`
- CP932/GBK 独立语料回归通过

### 重构批次 0 —— 缺陷登记簿 D1–D8 + 泄漏哨兵（2026-10-02，refactor-roadmap §4）

四路独立审计确认的 8 项真实缺陷全部修复（行为语义零变化，49/49 + 9/9 全绿）：

- **D1 安全**：nx.log 运行头命令行密码红线过滤（`-p`/`--password`/`--password-file` 值 → `***`）
- **D6 泄漏根因**：`LaSeqReader::replayQ_` 自引用环结构性消除——重放队列只存
  `{idx,name,meta}`，`next()` 重放现场重建条目源。环 = 15GB `nx-{GUID}.tmp` 残留案例根因
  （失败尝试的读取器永不析构，连带 spool/视图）；触发族两形态均有语料回归
  （`pw_retry_nested`：前置目录加密 zip 首轮错密码 + 无密码耗尽）
- **D7/决策 D-1**：过滤器链纳入 `--depth` 约束（每容器段链长 ≤ maxDepth，容器段重置），
  递归深度默认 8→10——原过滤器分支同 depth 无限递归，4MiB 嵌套 gzip 即可栈溢出（DoS）；
  语料 `filter_depth_bomb`（30 层 gzip → 退出码 3）、`depth_bomb` 加深至 12 层
- **D2**：`--depth/--max-ratio` 非法数值经 `from_chars` 全量校验 → 退出码 64（原 std::terminate）
- **D3**：`Stats::firstHardError` 封装 `HardErrorSlot`（自带互斥）——跨线程读写 std::string 的 UB
- **D4**：`win_long_path` 两次调用协议——超 1040 字符不再采纳未初始化栈缓冲（1248 字符实测）
- **D5**：`g_quiet` → `std::atomic<bool>`（fuzz/GUI/写出线程并发读写竞态）
- **D8**：SpoolBuffer::Reader 死代码删除 / 泵线程-QueueSource 构造顺序（OOM 死锁窗口）/
  `.Z` 后缀小写匹配 / `Sink::note` 走 `log_out`
- **哨兵 S1–S5**（`src/diag.hpp`，`NX_DIAG_LEAKS`，fuzz 常开）：spool/读取器活性注册表
  （fuzz 每迭代 + main atexit 全灭断言）、try_open 失败出口守卫、~ThreadPool/~Sink 析构纪律。
  校准：带环旧实现 + 前置目录加密 zip → S3 案发现场 abort；修复后同输入静默。
  诊断构建：`build-diag.cmd`（→ `build-diag\nx.exe`，`NX_DIAG_LEAKS_MAIN=ON`）

### 重构批次 1 —— 低风险速赢（2026-10-02，refactor-roadmap §8）

批次 0 之后按"纯核心/效果壳分离 + 消灭平行重复"推进（行为语义零变化，nxunit 235 + 50/50 + 9/9）：

- **M2**：filter.cpp 五解码器 RAII 化（ZlibDec/Bzip2Dec/LzmaDec/ZstdDec/Lz4Dec 适配器）+
  `pump_members<Dec>` 模板合并 gzip/bzip2/xz·lzma 三段逐字重复的多成员循环——
  20+ 处手工释放消失（含 zstd 初始化失败路径的原有泄漏）
- **M3**：LaSeqReader 抽 `pullBlock()`（readEntryData/readEntryDirect 两路径 ~40 行
  错误处理归一）
- **纯函数抽离（可单测）**：`detect_from_bytes`（嗅探逻辑与 peek 壳分离）、
  `eocd_from_window`/`parse_atom_header`（stego）、`render_report`（snapshot+render 分离，
  src/report.cpp）、`derive_exit_code`（退出码 if 链 → outcome.hpp）、`sanitize_rel`
  （dedupe 消毒半部）、`ascii_lower`（10 处手写循环统一）
- **领域类型**：`kFormatTable`（classify/format_name 唯一事实源）、`NameCodec`
  每读取器一份（原 g_nameCp 进程粘性跨包误判）、`AccessRecorder`（CbCtx 记录字段
  私有状态机化）、`Detection::sfxOffset/display()`（SFX 偏移结构化）
- **C++23 + Result**：`/std:c++23`；`Result<T> = std::expected<T, std::string>` 别名
  （试点：D2 数值参数解析；跨线程/熔断控制流仍走异常）
- **nxunit**：首个 C++ 单测目标（tests/unit_main.cpp，随主构建产出，run_tests.py
  以 `unit_core` 用例自动调用）——235 项纯函数断言

### 重构批次 2 —— 行为敏感：层身份 + 密码缓存键（2026-10-02，refactor-roadmap §8）

- **密码缓存键语义修正（含真实 bug 修复）**：`LayerId{key, display}` 拆分——缓存/游标键 =
  容器逻辑路径（`outer.tar.gz/a.tar.gz/data.zip`），提示/错误文本沿用"第 N 层 name (fmt)"。
  修复前键 = 深度+名：不同父容器下的同名分片组（a/b.tar.gz 各含 data.zip.001+）共享键，
  a 组耗尽候选把共享游标推过界 → **b 组连候选都不试即假性 PasswordExhausted**（实测 0 文件
  解出，候选里明明有 b 的密码）；语料 `sibling_pw_cache` 固化（51 用例）
- **LayerCtx**（src/layer.hpp）：{sub/origin/chain/logical/depth/filterChain/throughFilter}
  收敛 walk 原 6 个散参数；descend 规则唯一化（forEntry 进新容器段 / forFilter 链+1 逻辑
  路径不变——过滤器非密码层）；sub 受 --no-root 影响、logical 不受（层身份独立于输出布局）
- **Walker 类**（run/runStego/fsDirectOpen）+ `resolve_runtime_options` 装配单点
  （spool RAM 自适应 + 溢出目录默认从 main 迁入）

### 重构批次 3 —— 可测性（2026-10-02，refactor-roadmap §8）

- **MemorySource**（bytesource.hpp）：内存字节流（零拷贝直通），管线测试免文件系统——
  nxunit 现覆盖**真实路径集成**：多成员 gzip 经 filter_decode + BoundedQueue 背压解码、
  尾零容忍/截断/垃圾 → CorruptError、SFX 超首扫窗的 4MiB 补拉
- **PromptSink**（PasswordProvider::setPromptSink）：脚本化密码提示注入（携带层身份，
  空串=无输入）——密码解析链全语义可单测：候选顺序/每层游标独立/reportSuccess 三效/
  缓存先于候选/提示计数
- **select_group 拆分**：group_filesystem 决策半部抽纯函数（批次 1 顺延项补齐），
  文件系统接入只剩目录扫描 IO 壳；group_volumes/validate_set/select_group 三层单测
  （四命名体系/顺序陷阱/缺口告警/单卷退化/首卷意图）
- nxunit 达 **298 项检查**（批次 1 起 43 → 298），随主构建产出、run_tests.py 自动调用

### 重构批次 4 —— 契约与所有权（2026-10-02，refactor-roadmap §8）

- **前置验收门（形式化，TLA+/TLC 权威形态）**：`tools/ownership.tla` 三变体共用一份
  规范（legacy/weakOnly/fixed），GC 建模为独立原子 Collect 动作，TLC **完备**全状态
  空间检查（无深度上限）——legacy 违 NoLeak（反例 trace = Open→ProbePush→DropReader，
  即 replayQ_ 环触发族 1）；weakOnly 违 AsyncNoUseAfterDead（"weak 与 KeepAlive 必须
  配套"的形式化证明）；fixed 零违例。run_tests 以 `ownership_tla` 用例常驻（需
  Java + tools/tla2tools.jar）；`tests/ownership_model.py` 为 CI 快速门，结论须与
  TLC 一致。**方案过门后才动代码**
- **weak_ptr + KeepAlive**：LaEntrySource/szcom EntrySource 对读取器改持弱引用
  （父方向强边消除，强所有权图无环）；`ByteSource::keepAlive()` 虚令牌（别名构造），
  Sink 异步任务提交时捕获——任务可超出 walker 栈帧存活而读者不先亡
- **EntryToken 契约**（container.hpp）：迭代位置令牌三条款（重放只携带 {token,meta}、
  显式索取、读取器成员容器不得持条目源）
- **视图合并**：engines/szcom 两套逐行同构的 SeekView/SeekInput 家族 →
  `src/views.{hpp,cpp}` 唯一实现 + ViewFactory（挂表纪律从注释变命名调用：
  rootFile 挂 meter，probeFile/spool/region 不挂）
- **engines.cpp 拆分**：897 行 → namecodec / laimp（内部共享头）/ zipcd / laseq /
  open 五件，公共 API 不变
- **szcom cache_ 预算驱逐**（F5）：与 materializeBatch 共享 batchBudget 的真 LRU +
  字节记账；中读条目豁免（驱逐后重物化 pos 归零会内容错乱）

## 测试

```bash
python tests/gen_corpus.py       # 基础语料（含隐写 9 组 + 嵌套直读 1 组；tests/cases、tests/work 均在 .gitignore）
python tests/gen_corpus_m1.py    # M1 语料（zip/7z/rar；需 tests/tools/winrar/Rar.exe + 7z CLI）
python tests/gen_corpus_m2.py    # M2 语料（压缩比炸弹）
python tests/gen_corpus_fn.py    # 文件名编码语料（CP932/GBK）
python tests/run_tests.py        # 52/52（unit_core 298 项 + 所有权模型门 + 50 属性）
python tests/fuzz_run.py        # libFuzzer+ASan 全管线 fuzz（自动构建 build-fuzz/nxfuzz.exe；泄漏哨兵 S1-S5 常开——泄漏=abort=崩溃）
python tests/bench.py            # 基准（3 语料 × 3 方案）
python tests/gui_smoke.py        # GUI 冒烟 9 用例（窗口消息自动化）
```

## 已知限制与待办

| # | 问题 | 说明 | 优先级 |
|---|---|---|---|
| 1 | 嵌套隐写检测 | 隐写扫描仅根文件层（需 seek）；压缩包内的 MP4 不查（真实场景是右键单个文件） | 低 |
| 2 | 条目级分片连续到达 | M0 限制：分片组成员须连续到达，非成员条目到达即封组 | 低 |
| 3 | RAR4/旧命名卷 | WinRAR 7.x 无法生成 rar4 语料（读取由 7z.dll 覆盖，无测试验证） | 低 |
| 4 | tar 内符号链接 | v1 降级策略——跳过并告警，不落盘 | 低 |
| 5 | unRAR 插件 | 经评估略过（7z.dll 已覆盖 RAR 主线） | — |

（原待办「MP4 隐写压缩包识别」已完成：`extract-stego` 右键动词 / `--stego` 开关，见上「隐写解压」小节。）
