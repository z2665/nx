# 流式嵌套压缩包解压工具 —— 调研与设计文档

> 版本 v0.3 ｜ 2026-10-03 ｜ 平台：纯 Windows ｜ 实现：C++23（选型决策见 §7）
> 实现状态：M0–M3 + v1 后续 + 现代化重构批次 0–6 **全部完成**（55/55 测试 + GUI 9/9 +
> 合成发布门；进度与测试清单见 README，工作区纪律见 AGENTS.md，§10 为实施记录）
> v0.3 变更：语言标准更新为 C++23、工具链与依赖表对齐实现现状、案例代号匿名化（D-4）、
> §4.1 所有权与生命周期/§9.6 验证体系收编自重构路线图（该文件已退役删除）；历史决策记录（v0.2 选型）保留原貌。

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
- 上游顺序写入：前 N MiB（默认 64，`--spool-ram`）驻留 RAM，超出溢出到临时文件（`--temp-dir`，默认 `GetTempPath2` 所在盘）；
- 下游以随机访问读取，读写并发进行；引用计数归零即删除；
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
成员声明序显式化；COM 释放契约（S8 哨兵）。运行时哨兵见 `src/diag.hpp`（S1-S10，
`NX_DIAG_LEAKS`，fuzz 常开；诊断构建 `build-diag.cmd`）。

---

## 5. 关键设计决策

- **D1 内容检测优先于扩展名**：扩展名只用于分片排序和无 magic 格式（brotli）旁证。检测 = magic 表 + 轻量结构校验（tar 校验和、zip 本地头字段合法性、SFX 前缀内偏移扫描上限 64 MiB）。
- **D2 zip 双模式**：默认本地头流式；触发 SFX 前缀 / 流式读头失败 / 加密条目（走中央目录验证更稳）时自动回退 spool + 中央目录模式，对用户透明。
- **D3 分片在 ByteSource 层解决**（除原生 RAR 卷外），ConcatSource 虚拟拼接不落盘；条目级分片组复用同一套规则；RAR 原生卷走 7z.dll/unRAR 适配器。
- **D4 并发模型**：每条链的每个 FilterStage 一个线程，级间有界缓冲背压；容器条目间**顺序读**（本地头流式的本质约束），Sink 写出用线程池（默认 `min(8, cores/2)`）；互不共享底层流的不同分支整链并行。
- **D5 性能预算意识**：stored 条目零拷贝直通；缓冲 1 MiB 起步；线程池化避免每条目建线程；实测表明两级都快时管道开销会吃掉收益，压低这项开销是硬指标。
- **D6 安全（默认开）**：深度上限 10（决策 D-1，2026-10-02：原 8；容器嵌套深度与每容器段内过滤器链长共用此限——过滤器链无上界曾是 DoS 面，4MiB 嵌套 gzip 可栈溢出）；累计输出上限 512 GiB；单条目压缩比 >1000 告警/熔断；路径消毒（`..`、绝对路径、Windows 保留名 CON/NUL/COM1…、ADS 冒号、尾部点/空格、大小写不敏感重名）；符号链接默认降级；CRC 失败 `--keep-going` 隔离；先写 `.part` 临时名再原子 rename。
- **D7 加密与分层密码**：详见 §6——每层独立解析密码，支持"层 A 与层 B 密码不同"，密码不落日志。
- **D8 可观测性**：`--tree` 干跑嵌套结构树；`--progress` 树形进度 + 各级吞吐；`--report json` 输出层级/格式/耗时/校验结果（**不含任何密码信息**）。

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

## 7. 实现技术决策：C++（v0.2 选型 C++20 → 批次 1 升格 C++23）

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
| **libarchive** | tar/cpio/ar/zip（双模式）/cab/iso + 过滤器 gz/bz2/xz/zstd/lz4/.Z；zip ZipCrypto/AES、7z AES 读 | vcpkg（静态，overlay 两补丁见 README） | BSD |
| **zlib-ng[compat]**/bzip2/liblzma/zstd/lz4 | 过滤器直连（不经 libarchive，五解码器 RAII 适配） | vcpkg（静态） | 各自 |
| **7z.dll**（IInArchive COM） | 7z 全特性（**原生分卷** + AES + 头加密）+ RAR 解码兜底 | 运行时按需加载的独立 DLL | LGPL + unRAR 限制（独立 DLL 形态即满足隔离） |

未实现（v0.2 规划项，评估后搁置）：unRAR 可选插件（7z.dll 已覆盖 RAR 主线）、
brotli/.br（无 magic 试探解码，暂无真实需求）。

工具链：Visual Studio 2026、`/std:c++23 /MT`（v0.2 选型时为 C++20，批次 1 升格
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
5. **性能基准**：固定 3 组语料 × 3 方案（手工两遍 / bsdtar 管道 / 本工具）；指标 = 墙钟、**峰值磁盘占用**（流水线应为 0 中间）、峰值内存、用户 CPU。机械盘/网络盘环境必测（流水线收益放大器）。

### 9.6 验证体系（重构落地，run_tests 硬门）

- **双 TLA+ 模型**（缺 tla2tools.jar/java 直接 FAIL）：`tools/ownership.tla` 三变体
  ——legacy 复现 replayQ_ 反例、weakOnly 证"weak 与 KeepAlive 必须配套"、fixed 零违例；
  `tools/boundedqueue.tla` 三变体——closeNoWake/abandonNoWake 校准反例必违、
  fixed 零违例零死锁（DeadRelease + ParkedSanity）；
- **AST 强闭包检查器**（`tests/audit_ownership.py`，硬门）：clang-cl ast-dump →
  类→成员强边表 → **F\* 验证 + KaRaMeL 抽取的 closure_check.exe**（Closure.fst
  四引理全 VC）判定 esft 类成员强闭包含自身；校准 f647037 恰报 LaSeqReader
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
- **【已完成】GUI 进度指示（原待办 #1）**：独立 GUI 线程上的无模式进度对话框
  （输入文件名 + 当前活动行 + 动画条 + 已输出字节/文件数/耗时，200ms 定时轮询 `Stats`
  原子量），显示条件与完成弹窗一致（Explorer 启动或 `--gui`，非 tree）。
  取消按钮/X → `abortFlag` → Walker/Sink 抛 `Cancelled`（exit 2 静默退出，与密码弹窗取消同语义）；
  `Sink::writeOne` 大文件写出循环内逐块响应，`.part` 半成品照常清理。
  v1 用动画条而非百分比——根 zip（FileSeekView）与 7z.dll 直读路径绕过 `InputMeter`，
  真百分比需给两引擎接计量（已列入 README 待办）。
  `gui_smoke.py` 扩至 6 用例（出现/自动关闭/取消中止/半成品清理）。
- **【实施记录】进度条真百分比（原待办 #2，已随待办 #1 完成后补齐）**：
  语义 = 根输入消耗比 `meter.bytes / stats.inputTotal`。`InputMeter*` 经 `EngineOptions`
  透传（`Session::engineOpt()` 一处接线），三个挂点：zip 根 `FileSeekView`（正式/密码重试
  视图；码表探测视图不挂——多候选各重读一遍中央目录会虚增计数）、7z.dll 直读
  `FileSeekInput`（FS 卷；经 `SharedOpenState` 覆盖 RAR 多卷回调的后续卷）、流式根
  `FileSource`（原有）。spool 卷明确不挂（字节来自外层已计量流，再计即重复）。
  `inputTotal` 由 `run_input` 累计（单文件/分片组各卷之和，多输入累加）。
  重读造成的超出由显示端 99% 封顶吸收；分母未知回退动画条。
  附带修正：此前 zip 根下嵌套过滤器的压缩比熔断分母虚小（≈64 KiB 检测读），
  补计量后才是 D6 语义的真实根输入（ratio_bomb 回归通过）。
  冒烟断言 `PBM_GETPOS` 随解压爬升（zip 直读 0→77%、7z 直读 0→99% 实测）。
- **【已完成】MP4 隐写压缩包识别（原待办）**：入口为右键第三项 `extract-stego` /
  CLI `extract --stego`（用户显式选择"解隐写内容"→ 免运行时弹窗决策，语义=只解内藏
  压缩包、根文件本体不落盘；默认解压路径行为零变化）。检测两条路（`stego.cpp`，
  仅根 FS 层——流式 detect 无法跳过 GB 级 mdat）：① MP4 atom 步进（逐原子头小读、
  按 size 跳越，非法头处即候选起点；size=1 走 64 位扩展长度，size=0=延伸到 EOF；
  7z/rar 尾部无结束标记，只能经此发现）；② EOCD 反向扫描（末 64KiB+22 回扫
  `PK\x05\x06`，注释长度须精确吃到 EOF；覆盖任意格式尾接 zip 与 mdat size=0 病态）。
  打开：尾接 zip 整文件直开（libarchive 自 EOCD 反推 SFX 基址）；7z/rar 经
  `FileSeekInput` 的 `fsBase` 窗口（`[offset, EOF)` 呈现为完整卷）交 7z.dll。
  未命中 → exit 0 + GUI「nx 隐写解压」信息框；EOCD 假阳性由试开 CorruptError 兜回未命中。
  8 属性用例 + GUI 冒烟用例 8（43/43 + 8/8）。
- **【实施记录】真实隐写样本三重陷阱**（案例 S：2.5GB 隐写 MP4 真实案例，已匿名化，
  "7-Zip `#` 模式可开"）：
  ① EOCD 之后拖 18 KB 伪装数据，且文件末尾补一个 `size=8` 假 mdat 原子头——专门对付
  "EOCD 必须精确到 EOF"的尾部回扫类检测（libarchive 整文件直开也拒收）；
  ② zip 起点前有 76 字节诱饵（首见 `PK\x03\x04` 偏移 ≠ 真实基址）；
  ③ **zip64 影子值**——该档真值在 EOCD64+定位器，经典 EOCD 的 cdOffset/cdSize/条目数
  全是错的（写着 1 条目/90B，实际 4762 条目），EOCD 数学基址偏 76 字节。
  修复三层：EOCD 校验放宽（允许尾部伪装，区间 = [基址, EOCD 末尾)）；区间结果必须
  **自证**（算出的 CD 位置读 4 字节验 `PK\x01\x02`，不匹配即影子值 → 不信任）；
  不可信时回退**魔数锚点窗口**（首见 PK → 到 EOF），libarchive 自依 EOCD64 真值定位、
  且对窗口尾部残余数据有容忍（实测 [魔数, EOF) 与 [魔数, EOCD末尾) 均可解）。
  另踩一坑：EOCD 的 cdSize/cdOffset 是小端字段，与 MP4 atom 大端相反——已分设 be32/le32。
  样本病理已固化为语料（stego_disguise / stego_disguise_pw / stego_zip64_shadow）。
- **【实施记录】嵌套容器免 spool 窗口直读**（nested-window-direct-read 分支）：父为
  seekable 视图支撑且条目为 stored 时，子容器直接在父区间上随机访问，免全量 spool 往返。
  机制：CbCtx 记录条目数据相位的视图访问（seek/read 双记——read-ahead 缓冲命中时 read
  回调不触发）；regionOf 从首读位置回溯 512KB 定位本地头（PK\x03\x04+method==0+未加密+
  区间精确覆盖，zip 条目区间互不重叠保证唯一）；detect 后 walk 侧对容器条目 peek 1MiB
  促发底层读再取区间（peek 不消费零副作用）。任何失败（非 stored/加密/推导误判）自动回退
  spool 原路径——子打开失败即回退，安全性由兜底保证。
  边界实证：stored 父条目命中（300MB 嵌套 0.14s 免搬运）；deflate 父条目语义上无连续区间
  （澳洲女足 11GB 案例外层为 deflate 存储≈不可压缩数据）→ 正确回退 spool，行为与修复后
  基线一致。语料 nested_zip_stored（>2MiB 内层越过 256KB read-ahead 缓冲）覆盖命中路径。
  嵌套链 region 可链式套窗口（RegionView : SeekView : RegionSource）。
- **【实施记录】spool 溢出 4GiB DWORD 截断修复**（案例 M：11.23GiB 隐写 MP4
  真实案例，本地语料库，已匿名化）：SpoolBuffer::flushToTemp 整段落盘时
  `static_cast<DWORD>(ram_.size())`——RAM 环自适应至 8GiB 后首次触发（恰为
  2×4GiB，截断成 0），WriteFile 以长度 0 调用返回
  TRUE/写入 0 字节，落入 `wrote==0` 分支且 GetLastError()==0，报错文本竟为
  "写临时文件失败: 操作成功完成 (Win32 0)"。M0 起潜伏（旧固定 64MiB 环从未越过
  4GiB；另一 MP4 案例 2.5GB 亦侥幸）。修复：分块 ≤16MiB 落盘；全仓扫 DWORD 截断
  无同类。真实验证：解出 9.32GiB 双视频 exit 0（540s，含 8GB spool 往返）。
- **【实施记录】输出路径超 MAX_PATH + spool 临时文件残留**（案例 L：15GB 隐写
  MP4 真实案例——尾部伪装 + zip64 影子 EOCD → 隐写 zip（单条目 15GB 嵌套 zip，
  emoji+深中文路径）→ 内层 zip（32 条目 13.84GiB）；本地语料库，已匿名化，2026-10-01）：
  病灶①：最终路径 251 字符 + `.nxpart-<pid>-<tick>` 后缀≈267 > 260，
  Sink::writeOne 的 CreateFileW/MoveFileExW/DeleteFileW 裸路径调用——超 260 的
  裸路径报 ERROR_PATH_NOT_FOUND(3)（非"路径过长"，最易误诊），目录因
  ensure_dir_recursive 内部加 \?\ 前缀全部建成、仅文件失败。修复：三处统一
  win_long_path()（与目录创建同规范）。合成复现：337 字符路径 zip 报同错；
  语料 long_path（rel 271 字符，8×28 目录 + 40 文件名）回归。
  病灶②（附带）：成功运行后输出目录残留 14.86GB nx-{GUID}.tmp——退出转储证实
  spool 对象存活（LaSeqReader+SpoolSeekView 两强引用随 zip 读取器整体泄漏；
  小规模同构合成无法复现，触发面未完全定位）且强杀进程时 dtor 必然不执行。
  修复：溢出临时文件加 FILE_FLAG_DELETE_ON_CLOSE——句柄一关内核即删（独占句柄
  = 唯一持有者，spool 读经同一句柄无冲突），清理与对象生命周期解耦；同场景
  验证残留 0。读取器泄漏本体留待后续（影响仅内存壳，ram_ 溢出后已清空）。
- **【实施记录】solid 7z 批量抽取（O(N²) 修复）**（案例 XJ：2.61GB 隐写 MP4
  真实案例，本地语料库，已匿名化）：文件=MP4（尾部假 mdat + zip64 影子
  EOCD + 76B 诱饵，7-Zip 22.01 完全打不开）→ 隐写 zip（deflate 标记的不可压缩
  2.27GB 单条目 exe）→ 7z SFX（**Solid=+**，Delta+LZMA2:26+BCJ2+7zAES，3692 文件
  2.7GB，Ren'Py 游戏目录树）。病灶：szcom 逐条目单独 `arc_->Extract(&idx,1,…)` ——
  solid 块无独立寻址点，7z.dll 每次从头解码到目标位置，O(N×C/2)≈TB 级解码量；
  实测 10 分钟 322 文件且速率递减（0.63→0.30 文件/s），外推 8~12 小时。
  修复（pull 模型保持不变）：`materializeBatch(start)` 一次 `Extract` 携带
  [start,…) 一批连续数据条目（字节预算=spoolRam/2 钳 [64MiB,1GiB]，首条目必入批，
  条目数上限 4096），`ExtractCb` 批化——GetStream 按 index 分发到各条目独立
  SpoolBuffer，SetOperationResult 按 lastIdx 归属逐条目结果；全批成功逐条 finish 入
  cache_，批失败丢弃整批、仅单条重试被请求条目（坏点隔离，其后条目触发从自身开始的
  新批天然跳过坏点，keepGoing 语义与逐条目时代一致）。附带修复并发隐患：Sink 写出
  线程池并发 EntrySource::read → readEntry 对 `arc_->Extract`/`cache_`/
  InStreamImpl::pos_ 的无锁竞争（7z.dll IInArchive 单线程约定）——`mx_` 整体串行。
  理论：批预算 B 下重解码量 ≈ C²/2B（本例 B=1GiB → ~5GB，7z.dll 多线程解码 ~30s）。
  验证：新语料 7z_solid_many（600 文件×64KB，-ms=1g+AES）0.54s（逐条目时代分钟级，
  run_tests 加 <60s 时间断言防回退）；真实文件 38s/3747 文件/2.53GiB exit 0，
  与 7z CLI 单遍全解对比共同条目哈希零差异（多出的 66 文件=.save（Ren'Py 存档=zip）
  按设计递归展开；7z CLI 的 1 条 Warning 即同批矛盾路径）。fuzz 60s 回归无异常。
- **【实施记录】extract-into 同名冲突修复**（案例 Z：zip 直开真实案例，已匿名化）：默认前缀
  曾=完整文件名 → 输出目录与输入 zip 同名，`ensure_dir_recursive` 把 ALREADY_EXISTS
  误判成功（未验证是目录），到子条目目录创建才失败（错误仅在 stderr："创建目录失败"），
  表现为 GUI 解压 0 文件退出 1。修复两层：默认前缀改为去扩展名 stem（WinRAR 惯例，
  设计上避开撞名）；ensure_dir_recursive 对 ALREADY_EXISTS 验证 FILE_ATTRIBUTE_DIRECTORY。
  gui_smoke 用例 1 改为直接采用默认前缀（此前所有用例都覆盖了默认值，恰好漏掉该路径）。
  **无扩展名残余场景**（案例 X：1.85GiB 无扩展名 7z 嵌 7z 真实案例，内层加密，
  本地语料库，已匿名化）：
  文件名无小数点 → stem 回退=完整文件名 → 撞名仍发生；且创建失败在 main 提前 return 1，
  绕过完成弹窗，GUI 右键场景下用户完全看不到失败原因。补丁：失败按成因分类
  （`CompareStringOrdinal` 判撞输入文件/GetFileAttributes 判同名文件占位/其他含
  Win32 错误文本）+ GUI 交互流（extract-into/-stego 已弹过前缀窗、Explorer/`--gui` 启动）
  弹 `MessageBox` 给出换前缀指引；纯终端场景维持 stderr 不弹。gui_smoke 增用例 9
  （无扩展名输入 + 默认前缀 → 断言弹窗文案与 exit 1）。
- **【实施记录】性能四项（v1 后续）**：①zlib→zlib-ng[compat]（自建 overlay feature；inflate/CRC SIMD）；②spool RAM 自适应（空闲物理内存 50%，64MiB–8GiB）+ 溢出临时目录默认=输出目录（同盘零跨盘 I/O）；③libarchive nx-batch-ctr.patch：WinZip AES 每 16B单块 EVP（实测 ~60MB/s；OpenSSL 本体 AES-NI 10.8GB/s——瓶颈在调用粒度）→ 64KiB 批量 CTR，AES 2GiB 33.6s→2.8s 内容校验一致（首版两教训：批量 EVP 前须 EncryptInit_ex 重置、批后预生成状态跨批跳块——终版无预生成）；④bench：A -34%（反超 bsdtar）/ B -47%（恢复快于手工两遍）/ C -9%。补丁曾按上游风格提交 PR libarchive/libarchive#3443（overlay 与 PR 文本完全一致；3.8.7 与 master 该区域一字不差）——上游暂无 review 带宽已礼貌关闭，overlay 补丁持续生效，后续可重开或重提。
  PR 已含上游风格测试（aes128/256_multiblock：块边界尺寸 + 多批次 + 7B 流式/seek 读回逐字节校验；
  破坏性对照证实其能抓住跨批跳块类 bug；全量 866 用例无本补丁引入的失败，
  唯一失败的 test_archive_read_support_twice 在 pristine master 同样失败）。
- **【实施记录】文件名编码三层根因**（案例 N：本地 zip 真实案例，已匿名化）：
  ① libarchive 字符转换依赖进程 locale——C locale 下非 ASCII 名直接返回 NULL pathname，
  `setlocale(LC_ALL, ".UTF8")` 为主修复；② 本地头与中央目录文件名可不一致（本例本地头 EUC-JP、
  中央目录 UTF-8），流式读头必错——zip 已改中央目录模式（SeekView 免 spool 直读）；
  ③ `zip:hdrcharset` 候选须 iconv 名（CP932 非 932），码表探测按整包择一。
- **【实施记录】Fuzz 安全护城河**（自用定位下安全优先）：目标=全管线端到端——
  `src/fuzz_main.cpp` 每迭代写临时文件后走真实 `run_input`（detect/stego/容器引擎/密码链/
  Walker 递归/Sink 消毒），覆盖面=生产路径本身。MSVC libFuzzer+ASan（独立构建目录
  `build-fuzz/`、CMake `NX_FUZZ`；管线源与 nx 共用列表但禁 /GL），vcpkg 依赖非插桩——
  本仓代码带覆盖率/内存检测，依赖库硬崩溃仍被捕获。单迭代成本有界：深度 3 / 输出 2MiB /
  压缩比 50 / spool RAM 1MiB（促发溢出分支）；`stegoMode`/`noRoot` 由输入尺寸奇偶派生
  （libFuzzer 依赖确定性执行）；输出目录 4 槽轮换先清后用；进程内 `log_set_quiet` 静音
  （含 walker 层级列表的 printf，原来绕过 log 体系）。踩坑两枚：MSVC ASan 链的是
  **动态运行时 DLL**（`clang_rt.asan_dynamic-x86_64.dll`，post-build 复制到 exe 旁，否则
  非 VS 环境启动报 0xC0000141）；walker `layer_note` 直写 printf 绕过 quiet。首跑实测
  （5 分钟）：22,947 次 / 76 exec/s / 0 崩溃 / RSS 457MB，自动字典习得 CP936/CP932、
  各格式魔数、`ftyp` 等深层特征——码表探测与 stego atom 步进路径确被打到。
- **【实施记录】GUI 弹窗输入补齐**（自用体验）：① Win32 EDIT 原生不处理 Ctrl+A——
  子类化编辑框（`WM_CHAR` 0x01 → `EM_SETSEL 0,-1`），密码/前缀两弹窗共用；
  ② 弹窗自解压工作线程创建，常拿不到前台焦点——补 `WM_ACTIVATE`（非 WA_INACTIVE 即
  `SetFocus` 输入框），用户点活窗口后焦点直落输入框。窗口消息自动化不受影响
  （gui_smoke 8/8 照旧）。

---

## 11. 里程碑

| 阶段 | 内容 | 出口标准 | 状态 |
|---|---|---|---|
| M0（~2 周，C++20） | 工程骨架（vcpkg+静态 CRT）、ByteSource/有界队列/线程池、libarchive 引擎接入、嵌套 Walker、`.001` 拼接、密码：候选列表 + 交互 | 属性测试通过；3 层嵌套零中间文件；两层异密码用例通过 | ✅ 完成 |
| M1（+2 周） | zip 双模式回退、7z.dll（分卷+AES+头加密）、SpoolStore、全部分片类型（含 `.z01+.zip`、条目级分片组）、密码缓存/LRU | 对抗用例全绿（含密码专项） | ✅ 完成（unRAR 略过，7z.dll 已覆盖） |
| M2（+2 周） | 并发/背压调优、安全完备、`--tree/--report`、基准报告 | 基准不劣于 bsdtar 管道，峰值磁盘 0 中间 | ✅ 完成（链式快于手工 33-34%；附带发现 bsdtar 管道在 Windows 原生管道下解流式 zip 静默丢条目） |
| M3（正式发布） | unRAR 可选插件、`nxcore.dll` C ABI 导出、安装器/右键菜单、可选 WPF 壳 | 分发物 + 全量测试矩阵 | ✅ 完成（便携打包/右键级联/GUI 密码/默认日志；nxcore.dll 与 WPF 未做，非必需） |
| 后续迭代 | GUI 进度指示、MP4 隐写识别 | — | ✅ 均已完成（GUI 进度窗+真百分比、extract-stego 隐写解压，见 §10 实施记录） |
| v1 后续 | 性能四项（zlib-ng/spool 自适应/批量 CTR/LTO）、嵌套免 spool 直读、真实案例稳定性修复 | — | ✅ 完成（AES 12×、bench B 反超手工、stored 嵌套免搬运；见 §10 实施记录） |
| 现代化重构（2026-10-02/03） | 缺陷登记簿 D1-D8、领域类型化、纯核心/效果壳、所有权 DAG（weak_ptr+KeepAlive/EntryToken/res 圈禁）、验证体系（双 TLA+ 模型 / AST 闭包检查器 + F\* / clang-tidy 基线 / fuzz 哨兵 / 合成发布门） | 全量回归全绿 | ✅ 批次 0-6 全部完成（55/55 + 9/9；交付清单见 README「重构记录」，纪律见 AGENTS） |

---

## 附：调研来源

- libarchive 格式支持矩阵：[libarchive-formats(5)](https://man.archlinux.org/man/libarchive-formats.5.en)（zip 双读取器/加密、ISO 流式条件、多卷不支持）
- libarchive 多卷 RAR 故障：[GitHub issue #569](https://github.com/libarchive/libarchive/issues/569)
- unblob（嵌套提取先行者）：[github.com/onekey-sec/unblob](https://github.com/onekey-sec/unblob)、[unblob.org](https://unblob.org/)
- Rust 流式 zip（佐证本地头流式可行性）：[zip crate ZipStreamReader](https://strawlab.org/strand-braid-api-docs/latest/zip/unstable/stream/struct.ZipStreamReader.html)、[stream-unzip crate](https://lib.rs/crates/stream-unzip)
- PKZIP 分片顺序（.zip 为最后一卷）：[WinZip KB](https://kb.winzip.com/en/130798)、[Super User 讨论](https://superuser.com/questions/15935/how-do-i-reassemble-a-zip-file-that-has-been-emailed-in-multiple-parts)、[合并命令参考](https://askubuntu.com/questions/31298/how-to-extract-and-join-files-xxx-zip-xxx-z01-and-xxx-z02)
- 本仓库实测数据：见上一轮对话（nested_bench 实验，数据已清理，方法可复现）
