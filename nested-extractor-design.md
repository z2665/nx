# 流式嵌套压缩包解压工具 —— 调研与设计文档

> 版本 v0.4 ｜ 2026-10-04 ｜ 平台：纯 Windows ｜ 实现：C++23（选型决策见 §7）
> 工作区纪律见 AGENTS.md；开发指南（构建/源码地图/CI 硬门）见 DEVELOP.md；
> 进度与计划一律落 `plan/` 本地文件夹（gitignored，不入仓）——仓库文档只描述系统与开发方式，不记录开发状态。
> v0.4 变更：移除里程碑表与实施记录（历史细节见 git log 与 AGENTS 踩坑节）；其中机制性内容并入 §5 决策 D9/D10。
> v0.3 变更：语言标准更新为 C++23、工具链与依赖表对齐实现现状、案例代号匿名化（D-4）、
> §4.1 所有权与生命周期/§9.6 验证体系为后续补充节；历史决策记录（v0.2 选型）保留原貌。

---

## 1. 问题定义

**输入**：一个或一组文件，内部可能是任意深度的组合，例如：

```
data.zip.001/.002/.003           ← 分片
  └─ data.zip                    ← 外层 zip（可能加密）
      ├─ readme.txt
      └─ inner.7z                ← 嵌套 7z（可能用另一个密码）
          └─ bundle.tar.gz       ← 嵌套 tar.gz（本身又是两层）
              └─ photos.zip ...
```

**输出**：解开后的最终文件树（中间任何一层的归档文件本身默认不落盘）。

**核心目标**：
1. **自动识别**：按内容（magic bytes + 结构校验）识别每一层格式，不依赖扩展名（嵌套内容往往没有扩展名）。
2. **流式流水线**：各级解压以生产者-消费者并发执行，总时间 ≈ `max(各级耗时)` 而非求和，且中间文件零落盘（省磁盘、省 I/O）。
3. **分片感知**：识别各种分片命名体系，虚拟拼接后进入流水线。
4. **安全边界**：深度/总量/压缩比限制，路径消毒，防 zip bomb。
5. **密码感知**：任意层可加密，各层密码可以不同（§6）。

**已实测的性能依据**（本机 NVMe + 32 核，400MB 语料）：
- 串行两遍 0.35s → 管道并发 0.28s（省中间文件 I/O）；
- 慢外层（bzip2）场景 4.91s → 4.39s，内层 0.5s 耗时被完全并发吸收（`T1+T2 → max(T1,T2)` 的直接证据）；
- 两级都很快时（deflate ~1GB/s）收益被流式开销抵消——设计上必须做零拷贝直通与大缓冲来压低这项开销。

---

## 2. 现状调研：没有现成工具同时满足四点

| 工具/库 | 嵌套递归 | 分片感知 | 全程流水线(不落中间文件) | 多密码 |
|---|---|---|---|---|
| 手工管道 `tar -xOf a.zip b.zip \| tar -xf -` | ✗（人工） | ✗ | ✓ | ✗ |
| 7-Zip / WinRAR / PeaZip | ✗ | ✓ | ✗ | 每层手动 |
| `patool` / `atool` / `dtrx` | 部分 | 部分 | ✗ | ✗ |
| `unblob`（onekey-sec） | ✓（78+ 格式） | ✗ | ✗（每层先落盘再解） | ✗ |
| libarchive/bsdtar | ✗（单层） | ✗（明确不支持多卷；多卷 RAR 有已知故障 #569） | ✓ | 单层回调 |

**结论**：底层能力（流式解码、格式覆盖）已全部存在于各库中，缺的是一个**策略层**——分片虚拟拼接、按内容递归识别、流水线编排、分层密码、安全限制。这就是本工具的差异化定位。

---

## 3. 领域模型：格式三分 + 分片两类

### 3.1 压缩过滤器（单流 → 单流，纯流式，无 seek，均无加密概念）

| 格式 | magic | Windows 读取库 |
|---|---|---|
| gzip | `1F 8B` | zlib（libarchive 内置亦可） |
| bzip2 | `BZh` | libbzip2 |
| xz | `FD 37 7A 58 5A 00` | liblzma |
| lzma(裸) | `5D 00 00` | liblzma |
| zstd | `28 B5 2F FD` | libzstd |
| lz4(frame) | `04 22 4D 18` | lz4 |
| compress .Z | `1F 9D` | libarchive |
| brotli | **无 magic** | brotli 库 + 扩展名旁证/试探解码 |

注意：gzip/xz/bz2/zstd 都允许**多成员串联**，解码器必须循环读完整流，不能解完第一个成员就停。

### 3.2 归档容器 —— 按"读取是否需要 seek"分三类

| 类别 | 格式 | 说明 |
|---|---|---|
| **S：顺序容器** | tar（ustar/pax/gnu/v7）、cpio、ar | 天然流式。tar 无 offset-0 magic，靠 offset 257 的 `ustar` + 头部校验和验证；V7 老 tar 需"试解析" |
| **Z：尾部依赖容器** | zip | 本地头顺序流式可读，中央目录在末尾。流式模式处理不了 SFX 前缀、被追加/删改过的 zip → 自动回退 spool 模式 |
| **R：随机访问容器** | 7z（头可能在尾部且被压缩、实心块乱序）、RAR（实心卷）、ISO、CAB、WIM | 必须可 seek。走 SpoolStore（RAM 优先 → 磁盘溢出），**溢出写入与上游解压并发**，保住流水线收益 |

zip 流式读取必须处理的细节：数据描述符（GP bit 3）、zip64 扩展字段、加密标志位、本地头与中央目录不一致、SFX 偏移扫描、文件名编码（EFS 位 UTF-8 vs CP437）。

### 3.3 分片（多卷）—— 关键区分"字节拼接型"与"原生卷型"

| 命名体系 | 类型 | 处理方式 |
|---|---|---|
| `x.7z.001+.002…`、`x.zip.001…`、`x.tar.gz.001…`、裸 `x.001…` | **字节拼接型** | 按序号虚拟拼接成一个 ByteSource（`cat` 语义），格式无感知 |
| `x.z01, x.z02 … x.zip`（PKZIP/WinZip spanning） | 字节拼接型，**顺序陷阱** | **`.zip` 是最后一卷**（含中央目录），顺序 = z01…zNN, zip |
| `x.part1.rar, x.part2.rar…` / `x.rar + x.r00, x.r01…` | **原生卷型** | 每卷有独立头，不能拼接；交给 RAR 读取器（7z.dll / unRAR）原生跨卷 |
| GNU tar 多卷 | 原生卷型 | libarchive 明确不支持；v1 声明不支持 |

**分片发现的两个层级**：
1. **文件系统级**：同一目录内兄弟文件按上表规则分组；
2. **条目级**（"嵌套后分片"的常见形态）：分片作为多个条目散落在外层归档里（`outer.zip` 内含 `data.7z.001/.002`）——列出条目后先做同样的分组识别，再把这组条目的流虚拟拼接。

**完整性预检**：序号连续无缺口；除最后一卷外等长；拼接型 zip 可预扫 EOCD 定位校验；缺卷时报出缺失序号，而非解到一半失败。
**好性质**：分片本身不加密（密码属于内层格式），拼接层无需密码参与。

---

## 4. 总体架构

```
                     ┌────────────────────────────────────────────────┐
 文件系统/参数 ──►    │ VolumeSet 解析器（目录级 + 条目级两处接入）        │
                     └───────────────┬────────────────────────────────┘
                                     ▼
                     ┌───────────────────────────┐
                     │ ByteSource（唯一流抽象）    │  顺序读 + 可选回看 seek
                     │  FileSource / EntrySource  │  （回看 = RAM 环形缓冲
                     │  / ConcatSource(分片拼接)  │    → 磁盘溢出 spool）
                     └───────────────┬───────────┘
                                     ▼
                     ┌───────────────────────────┐
                     │ Detector（内容嗅探）        │  magic + 结构校验
                     └──────┬──────────────┬─────┘  + 扩展名辅助
              过滤器(§3.1)   │              │  容器(§3.2)
                     ▼              ▼
             ┌─────────────┐  ┌──────────────────────┐
             │ FilterStage │  │ ArchiveStage          │
             │ ByteSource  │  │ ByteSource → Entry流   │
             │   →ByteSrc  │  │ S/Z类: 直接流式        │
             │ (纯流式)    │  │ R类: 经 SpoolStore     │
             └──────┬──────┘  └──────────┬───────────┘
                    └──────► Walker ◄─────┘
                    （递归策略引擎 + Limiter + PasswordProvider）
                             │
                             ▼
                    ┌────────────────┐
                    │ Sink（安全落盘） │  路径消毒/重名/原子写
                    │ + 写出线程池     │  (.part 临时名 → rename)
                    └────────────────┘
```

### 核心抽象（C++）

```cpp
class ByteSource {                    // 唯一流抽象
public:
    virtual ~ByteSource() = default;
    virtual size_t read(std::span<std::byte> buf) = 0;   // 顺序读，阻塞
    virtual std::optional<uint64_t> rewindWindow() const { return {}; }
    virtual std::optional<uint64_t> sizeHint() const { return {}; }
};

class Filter {                        // gzip/xz/zstd/... 一层（纯流式）
public:
    virtual std::unique_ptr<ByteSource> decode(std::unique_ptr<ByteSource> src) = 0;
};

class ArchiveReader {                 // tar/zip/7z/... 一层
public:
    // 惰性条目流：读一个头出一个条目；加密层先向 PasswordProvider 取密码
    virtual std::unique_ptr<EntryIter> entries(std::unique_ptr<ByteSource> src,
                                               PasswordProvider& pw) = 0;
};

struct Entry {
    LogicalPath path;                 // 嵌套树中的逻辑路径（决定输出位置与密码层身份）
    std::shared_ptr<ByteSource> data; // 条目数据流（同容器条目间顺序共享）
    std::optional<Format> kindHint;   // 扩展名线索（仅辅助嗅探）
    EntryMeta meta;                   // 大小(可能未知)/CRC/权限/加密标志
};
```

**关键机制：SpoolStore（R 类格式的 seek 适配器）**
- 上游顺序写入：RAM 驻留自适应（默认=空闲物理内存 50%，64MiB–8GiB，`--spool-ram` 覆盖），超出溢出到临时文件（`--temp-dir`，默认=输出目录——同盘零跨盘 I/O）；
- 溢出写入与上游解压**并发**（append 随解码推进，流水线收益保住的那一半）；下游随机访问在 `finish()` 之后进行（R 类引擎开卷本就需完整数据）；
- 溢出临时文件 `FILE_FLAG_DELETE_ON_CLOSE`——句柄一关内核即删，清理不依赖对象生命周期（15GB 残留案例教训）；对象存活由 `shared_ptr` 引用管；
- 这是 7z/RAR/ISO 进入统一流水线的桥梁——只给真正需要 seek 的格式用，且不损失与上游的并发。

**Walker 的分支语义**：容器可能同时含普通文件、嵌套归档、分片组。Walker 对条目列表先做分片分组，再逐条目嗅探：普通文件 → Sink；嵌套归档/分片组 → 递归（受 Limiter 与密码解析约束）；嗅探不置信且扩展名无线索 → 按普通文件落盘（`--aggressive` 才试探解码）。

**Windows 集成要点**：
- 长路径统一 `\\?\` 前缀（清单声明 `longPathAware`）；
- `CreateFile` 加 `FILE_FLAG_SEQUENTIAL_SCAN`（读卷）/ 写临时文件后 `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` 原子落名；
- 控制台 `SetConsoleOutputCP(CP_UTF8)` + VT 转义进度（Win10+ 终端原生支持）；
- 磁盘水位：`GetDiskFreeSpaceEx` 预检 + 解压中周期复查，超限熔断；
- 线程模型：`std::jthread` + `stop_token`，级间固定容量 MPSC 有界队列（默认 1 MiB `--buffer`）形成背压——内存上界 = 活跃链数 × 级数 × 缓冲，可预测。不引入协程，普通线程池足够且更好调试。

### 4.1 所有权与生命周期（重构后纪律，权威定义）

强所有权边构成以 {main 栈 Session、Walker 活动栈帧、Sink 任务队列、7z.dll COM 引用}
为根的 **DAG**（无环由三层验证保证，见 §9.6）。核心机制：

- **weak_ptr + KeepAlive 令牌（配套，缺一即违）**：条目源（LaEntrySource/EntrySource）
  对读取器只持 `weak_ptr`；异步写出的存活由 Sink 任务捕获 `keepAlive()` 保活令牌；
- **EntryToken 契约**（container.hpp）：迭代位置显式令牌——重放只携带 {token,meta}、
  显式索取、读取器成员容器不得持有条目源或含条目源的类型（INV-3）；
- **P2 圈禁**：Win32 句柄/COM 引用/临时文件的 RAII 唯一来源在 `src/res/`（释放
  五名单硬门，见 AGENTS 架构分层节）；文件句柄接入一律 `res::adopt_file()`；
- **生命周期状态机与关键不变式**：

| 类型 | 状态机 | 关键不变式 |
|---|---|---|
| SpoolBuffer | growing → finished（overflowed 单调） | read_at 仅 finished；append 仅 growing；overflowed ⇒ ram_.empty() |
| LaSeqReader | probing → iterating → drained → dead | dead 时 replayQ_ 必空；adoptStream 至多一次 |
| SevenZipReader | opening⇄enumerate⇄probe → ready → iterating → dead | arc_≠null ⇒ ready/iterating；cache_[i].pos 单调 |
| Sink/ThreadPool | accepting → draining → destroyed | destroyed 仅可自 draining 迁入（先 waitAll） |
| BoundedQueue | open/closed → dead（终态） | dead 时两侧必不阻塞（TLA+ DeadRelease 已证） |

借用纪律断言化（四条）：Sink 析构前必 waitAll；QueueSource 先亡于队列；Session/Sink
成员声明序显式化；COM 释放顺序契约（szcom tryOpen 的 P6 注释契约——`arc.reset()`
先于 `mainStream_.reset()`，无运行时哨兵，改动释放序时人工对照）。运行时哨兵见
`src/diag.hpp`（S1-S5，`NX_DIAG_LEAKS`，fuzz 常开；诊断构建 `build-diag.cmd`）。

---

## 5. 关键设计决策

- **D1 内容检测优先于扩展名**：扩展名只用于分片排序和无 magic 格式（brotli）旁证。检测 = magic 表 + 轻量结构校验（tar 校验和、zip 本地头字段合法性、SFX 前缀内偏移扫描上限 64 MiB）。
- **D2 zip 双模式**：默认本地头流式；触发 SFX 前缀 / 流式读头失败 / 加密条目（走中央目录验证更稳）时自动回退 spool + 中央目录模式，对用户透明。
- **D3 分片在 ByteSource 层解决**（除原生 RAR 卷外），ConcatSource 虚拟拼接不落盘；条目级分片组复用同一套规则；RAR 原生卷走 7z.dll/unRAR 适配器。
- **D4 并发模型**：每条链的每个 FilterStage 一个线程，级间有界缓冲背压；容器条目间**顺序读**（本地头流式的本质约束），Sink 写出用线程池（默认 `min(8, cores/2)`）；互不共享底层流的不同分支整链并行。
- **D5 性能预算意识**：stored 条目零拷贝直通；缓冲 1 MiB 起步；线程池化避免每条目建线程；实测表明两级都快时管道开销会吃掉收益，压低这项开销是硬指标。
- **D6 安全（默认开）**：深度上限 10（决策 D-1，2026-10-02：原 8；容器嵌套深度与每容器段内过滤器链长共用此限——过滤器链无上界曾是 DoS 面，4MiB 嵌套 gzip 可栈溢出）；累计输出上限 512 GiB；**spool 磁盘溢出总量同限**（决策 2026-10-05，红队 M6：R 类嵌套的中间字节不进输出侧预算、溢出相位曾无上界——204KB 输入实测可推 200MB spool；RAM 相位仍由 `--spool-ram` 自界，溢出每次写后查总量 + 水位复查，余量 < 待写+64MiB 即熔断，避免 WriteFile 中途 disk-full 的原始错误）；单条目压缩比 >1000 告警/熔断；路径消毒（`..`、绝对路径、**两种分隔符（`/` 与 `\` 同为段边界——混合分隔符穿越曾击穿只按 `/` 分割的消毒器，2026-10-05 红队 C1）**、Windows 保留名 CON/NUL/COM1…、ADS 冒号、尾部点/空格、大小写不敏感重名）；符号链接默认降级；CRC 失败 `--keep-going` 隔离；先写 `.part` 临时名再原子 rename。
- **D7 加密与分层密码**：详见 §6——每层独立解析密码，支持"层 A 与层 B 密码不同"，密码不落日志。
- **D8 可观测性**：`--tree` 干跑嵌套结构树；`--progress` 树形进度 + 各级吞吐；`--report json` 输出层级/格式/耗时/校验结果（**不含任何密码信息**）。
- **D9 隐写解压（extract-stego / --stego，显式动词）**：只解根文件内藏的压缩包，根文件本体不落盘；未命中 exit 0 + 提示，默认解压行为零变化。检测两条路（仅根 FS 层——流式 detect 无法跳过 GB 级 mdat，需 seek）：① MP4 atom 步进——逐原子头小读、按 size 跳越，非法头处即候选起点（size=1 走 64 位扩展长度，size=0 延伸到 EOF；7z/rar 尾部无结束标记只能经此发现）；② EOCD 反向扫描——末窗口回扫 `PK\x05\x06`，不要求精确到 EOF（允许尾部伪装），区间结果须 CD 签名 `PK\x01\x02` 自证，不可信（zip64 影子值）则回退魔数锚点窗口由 libarchive 依 EOCD64 真值定位。打开：尾接 zip 走 FileSeekView 精确窗口；7z/rar 走 fsBase 窗口交 7z.dll；EOCD 假阳性由试开失败兜回未命中。
- **D10 嵌套容器免 spool 窗口直读**：父视图 seekable 且条目 **stored** 时，嵌套 zip/7z/rar 直接在父区间随机访问（RegionView 可链式套窗口），免全量 spool 往返。机制：数据相位的 read+seek 双记（libarchive 256KB read-ahead 缓冲命中时 read 回调不触发，seek 是唯一信号）→ 从首读位置回溯 512KiB 定位本地头 → **本地头自证**：魔数 + 未加密 + method==0 之外，还须 `csize==usize==条目尺寸`——压缩流/相邻结构里偶合出现的 stored 头（典型：嵌套 zip 自己的 stored 目录条目原样出现在外层 deflate 的 stored 块中）在此排除，曾因缺自证推出错位区间致子打开中途损坏、且触发随流字节巧合漂移 → 区间直读。安全网：任何失败（deflate 父条目/加密/推导误判/子打开失败）**自动回退 spool 原路径**——回退语义是硬边界（AGENTS 行为红线）。

---

## 6. 加密与分层密码设计（v0.2 新增）

### 6.1 各格式加密能力

| 格式 | 加密方式 | 读取引擎 | 错误密码的判定 |
|---|---|---|---|
| zip | ZipCrypto（弱，已知明文可破）/ WinZip AES-256 | libarchive（两者均可读） | ZipCrypto 12 字节校验头；AES 2 字节验证码 |
| 7z | AES-256，**可加密文件头/文件名** | 7z.dll / libarchive | 头部 CRC 校验失败 |
| rar | RAR3/RAR5 AES | 7z.dll / unRAR | per-file 校验失败 |
| gzip/tar/分片 | 无加密概念 | — | — |

注意 7z 头加密的连带影响：**连条目列表都拿不到**，必须先有密码才能枚举——所以密码解析必须发生在 `ArchiveReader::entries()` 之前，这是接口里 `PasswordProvider` 出现在该位置的原因。

### 6.2 PasswordProvider：每层独立的解析链

**层的身份 = 逻辑路径 + 格式**（如 `data.zip#inner.7z`），同工具内不同层各自独立解析，天然支持"两层密码不一致"。

每层按下述顺序取密码，任一成功即止：

```
1. 缓存命中        —— 本进程内该层已成功的密码（重试/续传场景）
2. 上次成功密码     —— 全局 LRU（人们常对多层的压缩包复用同一密码，先试它最省）
3. 候选列表顺序尝试 —— -p/--password（可重复）与 --password-file（每行一个）
                       依次注入引擎，凭 6.1 的错误判定区分"密码错"与"数据坏"
4. 交互询问        —— 仅 TTY；提示必须带层身份：
                       "第 2 层 inner.7z (AES-256) 的密码："
                       回显关闭（ReadConsole 关 ECHO），Ctrl+C 安全退出
5. 全部失败        —— 该分支标记失败并继续其余分支（--keep-going 语义默认对密码生效）
                     非交互场景（重定向/CI）自动跳过第 4 步（--no-prompt）
```

**UX 示例（两层不同密码）**：

```
> nx extract data.7z.001 -O out -p CommonPw@2024
[1] data.7z … AES-256 → 候选#1 验证通过
[2]   inner.zip … AES-256 → 候选#1 失败(校验码不符) → 候选#2 无 → 询问
      第 2 层 inner.zip 的密码：********
[2]   inner.zip … 密码验证通过
✓ 1,247 个文件，3 层全部解开，中间文件 0 字节
```

**试探成本**（决定候选列表可用规模）：WinZip AES 每次试探 = PBKDF2-HMAC-SHA1×1000 ≈ 0.1ms 级；7z AES 同量级；**RAR3 例外**（变长 KDF，单次试探可达几十 ms），RAR 层的候选列表建议 ≤100 条。错误判定必须区分"密码错"与"数据损坏"（前者换下一个候选，后者直接报损坏），引擎错误码要做映射表。

### 6.3 密码安全纪律

- 密码存放于 `std::unique_ptr<std::byte[]>` 固定容量缓冲，用完 `SecureZeroMemory`；**禁止** `std::string`（堆上残留、SSO 不可控）；
- 不写日志、不进 `--report`、不出现在进程命令行回显以外的任何地方；命令行传密码本身可被同机进程窥见（Win32 进程可读他进程命令行），文档明示风险并推荐 `--password-file` 或交互输入；
- 错误密码不做延时惩罚（这是解压工具不是登录系统），但在 report 中记录"该层试探 N 次"便于审计；
- `--password-file` 权限建议仅当前用户可读（创建时即设）。

---

## 7. 实现技术决策：C++（v0.2 选型 C++20，后升格 C++23）

### 7.1 C++20 vs .NET 8 决策记录

| 维度 | C++20 | .NET 8 (C#) |
|---|---|---|
| 核心引擎亲和度 | libarchive/7z SDK/unRAR 全是原生 C/C++ API，密码回调、流回调直连，**零 interop** | 全部要走 P/Invoke 或 COM；7z.dll 的 COM interop 尤其繁琐 |
| 分发 | 静态链接 `/MT` → **单 exe 约 3~5 MB**，无运行时依赖 | self-contained 单文件 ~70 MB，或要求用户装运行时 |
| 性能上限 | 流水线热路径全原生；性能本身就是本工具卖点 | 托管层开销小但存在；interop 边界在热路径上 |
| 解析不可信数据 | 需自律（好在重活都在 C 库里，策略层可用 span/RAII 收紧） | 内存安全天然优势 |
| 开发速度 | 慢约 2×，依赖管理用 vcpkg 化解 | 快，`System.Threading.Channels` 写管道很顺手 |
| GUI 后路 | 核心导出 C ABI DLL，壳另做 | WPF/WinUI 原生顺手 |

**决定：核心与 CLI 用 C++（选型时 C++20，现为 C++23——std::expected 承接 Result 别名，ABI 无影响）。** 理由：① 这个工具的本质是"原生解码库的编排器"，三个关键引擎全是 C/C++ API，选 .NET 意味着所有重活仍然经过 interop，只省了策略层开发量；② 纯 Windows 定位下 .NET 的跨平台优势归零；③ 单文件小体积对"右键菜单/脚本/绿色软件"形态的 Windows 工具是实际竞争力。
**保留后路**：核心做成 `nxcore.dll`（纯 C ABI），未来 GUI 用 C# WPF 薄壳调用——两全。若团队 C# 产能远高于 C++，可切换到".NET 壳 + P/Invoke libarchive"方案，架构文档其余部分不受影响。

### 7.2 依赖与引擎分工（对齐实现现状）

| 引擎 | 职责 | 引入方式 | 许可 |
|---|---|---|---|
| **libarchive** | tar/cpio/ar/zip（双模式）/cab/iso + 过滤器 gz/bz2/xz/zstd/lz4/.Z；zip ZipCrypto/AES、7z AES 读 | vcpkg（静态，overlay 两补丁见 DEVELOP） | BSD |
| **zlib-ng[compat]**/bzip2/liblzma/zstd/lz4 | 过滤器直连（不经 libarchive，五解码器 RAII 适配） | vcpkg（静态） | 各自 |
| **7z.dll**（IInArchive COM） | 7z 全特性（**原生分卷** + AES + 头加密）+ RAR 解码兜底 | 运行时按需加载的独立 DLL | LGPL + unRAR 限制（独立 DLL 形态即满足隔离） |

未实现（v0.2 规划项，评估后搁置）：unRAR 可选插件（7z.dll 已覆盖 RAR 主线）、
brotli/.br（无 magic 试探解码，暂无真实需求）。

工具链：Visual Studio 2026、`/std:c++23 /MT`（v0.2 选型时为 C++20，后升格
C++23 承接 std::expected）、vcpkg manifest 固定版本（供应链审计友好）、CMake + Ninja。

---

## 8. CLI 设计

```
nx extract data.zip.001 -O out/                 # 全自动：分片→嵌套→并行落盘
nx extract outer.zip -O out/ --depth 3          # 限制递归深度
nx tree  outer.tar.gz                           # 只看嵌套结构，不落盘
nx extract x.7z.001 -p pw1 -p pw2 \
          --password-file pws.txt --no-prompt   # 脚本场景：仅候选列表
nx extract x.zip --max-bytes 100G --jobs 8 --keep-going
nx extract x.zip --verify sha256 --report r.json
nx extract x.zip --spool-ram 256M --temp-dir D:\fast\
```

退出码区分：成功 / 部分失败 / 密码缺失或耗尽 / 超限熔断 / 缺分片。输出目录镜像逻辑嵌套树（外层名为顶层目录，过滤器层合成目录），`--flatten` 拍平。

---

## 9. 测试与基准

1. **语料生成器**：可控参数 = 压缩比、文件数与大小、嵌套深度与格式组合、分片大小、SFX/加密开关、**密码组合**（单层/多层同密码/多层异密码/错误候选顺序）。
2. **属性测试（正确性金标准）**：任何生成组合，本工具输出 ≡ 逐层手工解压（7-Zip/bsdtar 循环）——全树哈希对比。
3. **对抗用例**：zip-slip 路径集、Windows 保留名/ADS/超长名、42.zip 风格炸弹（熔断必须触发）、截断流、坏 CRC、缺分片、序号跳号、`.z01+.zip` 顺序陷阱、本地头/中央目录不一致、**7z 头加密（无密码连列表都没有）**、错误密码与损坏数据混叠。
4. **密码专项**：层身份正确性（同密码复用命中缓存；异密码各自询问且提示文本带层路径）；回显关闭；`--no-prompt` 下密码耗尽的退出码；RAR3 大候选列表的耗时长尾。
5. **性能基准**：固定 3 组语料 × 3 方案（手工两遍 / bsdtar 管道 / 本工具）；指标 = 墙钟、**峰值磁盘占用**（流水线应为 0 中间）、峰值内存、用户 CPU。机械盘/网络盘环境必测（流水线收益放大器）。注意：Windows 原生管道下 bsdtar 解流式 zip 存在静默丢条目问题——bsdtar 管道方案的对比数据仅作参考。

### 9.6 验证体系（重构落地，run_tests 硬门）

- **双 TLA+ 模型**（缺 tla2tools.jar/java 直接 FAIL）：`tools/ownership.tla` 三变体
  ——legacy 复现 replayQ_ 反例、weakOnly 证"weak 与 KeepAlive 必须配套"、fixed 零违例；
  `tools/boundedqueue.tla` 三变体——closeNoWake/abandonNoWake 校准反例必违、
  fixed 零违例零死锁（DeadRelease + ParkedSanity）；
- **AST 强闭包检查器**（`tests/audit_ownership.py`，硬门）：clang-cl ast-dump →
  类→成员强边表 → **F\* 验证 + KaRaMeL 抽取的 closure_check.exe**（Closure.fst
  四引理全 VC）判定 esft 类成员强闭包含自身；校准 2d20794 恰报 LaSeqReader
  零误报；同脚本含 P2 圈禁 grep 门（五名单 + 每次运行正/负样本自检）；
- **clang-tidy 基线门**（`tests/tidy_check.py` + `.clang-tidy`）：owning-memory/
  dangling-handle/mt-unsafe/unnecessary-value-param 四检查零警告；
- **fuzz**（libFuzzer+ASan，哨兵 S1-S5 常开）+ `tests/fuzz-regression/` 持久回归
  种子（历史崩溃工件，populate_seeds 回灌）；
- **发布门**（`tests/release_gate.py`）：合成语料（真实案例结构重建，确定性种子，
  D-4 隐私纪律）端到端哈希比对，manifest 入库。

三层分工：**TLA+ 管设计语义、AST 检查器管代码现状、F\* 管检查器不说谎**。
MSVC /analyze 排雷（`build-analyze.cmd`，低噪子集）为非门辅助，项目源零警告。

---

## 10. 风险与开放问题

- **RAR 许可与覆盖**：rar5 + 实心 + 恢复记录只有官方系稳；以独立 DLL（7z.dll 自带 / unRAR 插件）隔离。
- **libarchive 对 rar5 加密支持有限** → 7z.dll 路径兜底。
- **条目级流式 zip 的固有盲区**（追加修改、SFX）由 D2 双模式兜底；检测阶段前置判断，防回退风暴。
- **超大单文件**（>4 GiB）的 zip64/7z/tar 边界测试量大。
- **Windows 文件名合规**（大小写不敏感碰撞、尾部点/空格）需专门消毒器。
- **是否做写入方向**（顺手把嵌套包重打包为单层 tar.zst）v2 评估。
- **多成员 gzip/xz 串联**与"分片切在成员边界附近"的组合易错，语料生成器显式覆盖。

---

## 附：调研来源

- libarchive 格式支持矩阵：[libarchive-formats(5)](https://man.archlinux.org/man/libarchive-formats.5.en)（zip 双读取器/加密、ISO 流式条件、多卷不支持）
- libarchive 多卷 RAR 故障：[GitHub issue #569](https://github.com/libarchive/libarchive/issues/569)
- unblob（嵌套提取先行者）：[github.com/onekey-sec/unblob](https://github.com/onekey-sec/unblob)、[unblob.org](https://unblob.org/)
- Rust 流式 zip（佐证本地头流式可行性）：[zip crate ZipStreamReader](https://strawlab.org/strand-braid-api-docs/latest/zip/unstable/stream/struct.ZipStreamReader.html)、[stream-unzip crate](https://lib.rs/crates/stream-unzip)
- PKZIP 分片顺序（.zip 为最后一卷）：[WinZip KB](https://kb.winzip.com/en/130798)、[Super User 讨论](https://superuser.com/questions/15935/how-do-i-reassemble-a-zip-file-that-has-been-emailed-in-multiple-parts)、[合并命令参考](https://askubuntu.com/questions/31298/how-to-extract-and-join-files-xxx-zip-xxx-z01-and-xxx-z02)
- 本仓库基准数据可由 `python tests/bench.py` 复现（方法即 §9.5）
