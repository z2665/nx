# nx — 流式嵌套压缩包解压工具

设计文档：[nested-extractor-design.md](nested-extractor-design.md)（v0.2）。当前实现状态：**M3 完成**（M0 流水线 + M1 zip/7z/rar 全格式 + M2 调优/基准 + M3 便携分发/右键菜单/GUI/日志）。

## 构建（Windows + VS 2026 + vcpkg）

```cmd
build.cmd
```

脚本内部：vcvars64 → CMake(Ninja) → vcpkg manifest 安装（`x64-windows-static`，静态 CRT）→ 编译。
产物 `build\nx.exe`：约 5 MB 单文件，仅依赖 Windows 系统 DLL。

依赖（vcpkg manifest 固定）：libarchive 3.8.7（tar/cpio/ar/zip/iso/cab + 兜底）、
zlib/bzip2/liblzma/zstd/lz4（过滤器直连）。
注意 `ports-overlay/libarchive`：上游 CMake 未链 crypto 探测 `PKCS5_PBKDF2_HMAC_SHA1` 失败，
导致 WinZip AES 解密被编译为 stub——overlay 强制定义该宏修复。

**7z.dll**（运行时按需加载，不随构建）：搜索顺序 = exe 同目录 → `C:\Program Files\7-Zip\` → PATH。
负责 7z 全特性（AES + 头加密 + 分卷）与 RAR 解码（rar4/rar5、加密、原生多卷）；缺失时自动回退 libarchive（多卷 RAR 除外）。

## 测试

```bash
python tests/gen_corpus.py    # M0 语料（14 组）
python tests/gen_corpus_m1.py # M1 语料（zip/7z/rar；需 tests/tools/winrar/Rar.exe 与 7z CLI）
python tests/run_tests.py     # 属性测试：25/25 通过
```

WinRAR 控制台工具位于 `tests/tools/winrar/`（试用版解包使用，仅语料生成；git 忽略，勿分发）。
WinRAR 7.x 已移除 RAR4 创建（`-ma4`/`-vn`），故 rar4 读取路径由 7z.dll 覆盖但无生成器可用例。

### 用例覆盖

- **M0**：三层嵌套零中间文件、`.001`/`.z01+.zip` 分片、条目级分片、多成员 gzip、zip-slip、
  坏 CRC、深度熔断、缺分片、裸过滤器、双层异密码 zip
- **M1**：rar5/solid/头加密(-hp)/新式多卷(.partN.rar)/条目级多卷、7z 内容加密/头加密(-mhe)/
  字节拼接分卷(.7z.001)、zip SFX（魔数扫描 + D2 回退）、
  **zip→7z(密码A)→rar(密码B) 三格式异密码嵌套链**

## M2：并发/背压调优、安全完备、--tree/--report、基准

- **性能（D5）**：PushbackSource 游标化 + 检测后关闭历史（直通模式）+ `read_direct` 零拷贝链
  （libarchive 块视图 → WriteFile 直写，全程无中间 memcpy）；FS 级 7z/rar 根文件免 spool 直读；
  7z.dll 惰性加载；Sink 写出线程池（独立 spool 源异步写，D4）
- **安全（D6）**：压缩比熔断（产出/输入 > max-ratio → exit 3；分母含根尺寸提示，
  小输入炸弹也能判定）；每文件磁盘水位复查
- **可观测（D8）**：`--verify sha256`（BCrypt，输出哈希入报告）、`--report r.json`
  （统计/耗时/校验，不含任何密码信息）
- **基准（§9.5，`python tests/bench.py`）**：256MB 语料 × 3 方案。
  链式用例（tar.gz→zip、tar.bz2→zip）nx 快于手工两遍 33-34%，峰值中间磁盘 0 MiB
  （手工两遍 +18 MiB），输出内容与基准一致。
  附带发现：**bsdtar 管道在 Windows 原生管道下解流式 zip 会静默丢条目**（可复现，
  python/cmd 管道均然）——其墙钟不能作为有效对照，正是设计 §2 描述的现成工具缺陷。

## M3：便携分发、右键菜单、GUI、默认日志

- **便携打包**：`package.cmd` → `dist
x\`（nx.exe + 7z.dll + README + LICENSE），
  免安装绿色版；右键菜单命令指向该目录的绝对路径（移动目录后需重新 install）
- **右键菜单**（HKCU，免管理员）：`nx menu install | remove`，级联菜单「nx 解压」：
  - *解压到当前目录*：`extract-here`——条目直接落在压缩文件所在目录（--no-root）
  - *解压到指定目录…*：`extract-into`——GUI 询问前缀（默认=压缩文件名），
    输出 `<所在目录>\<前缀>`；X/取消即中止退出（exit 2）
- **GUI 密码弹窗**：无控制台（右键/资源管理器启动）或 `--gui` 时，遇到加密层自动弹窗，
  每层一窗、标题带层身份；取消/关闭 = 中止整个任务
- **默认日志**：每次运行写 `nx.exe` 所在目录 `nx.log`——运行头（时间/pid/命令行）、
  全部输出、report JSON；始终 append，超 5 MiB 截断从 0 开始
- **双模式 exe**（`/SUBSYSTEM:WINDOWS`）：资源管理器右键启动不闪控制台黑框；
  终端/管道启动时行为与普通 CLI 完全一致

## 用法

```
nx menu install                          # 注册右键菜单（当前用户）
nx extract data.zip.001 -O out/ -p pw1 -p pw2 --no-prompt
nx extract mv.part1.rar -O out/            # RAR 原生多卷自动聚合
nx extract x.zip -O out/ --verify sha256 --report r.json
nx tree  outer.tar.gz
```

退出码：0 成功｜1 部分失败｜2 密码缺失或耗尽｜3 超限熔断｜4 缺分片｜64 用法错误。

## 源码布局（src/）

| 模块 | 职责 |
|---|---|
| `bytesource.*` | 唯一流抽象：File/Concat/Queue/Pushback（回看窗口） |
| `pipes.*` | 有界队列（背压）+ 线程池 |
| `spool.*` | SpoolStore：RAM 优先→临时文件溢出，随机访问/窗口视图 |
| `detect.*` | magic + 结构校验（tar 校验和）+ SFX 前缀魔数扫描（D1） |
| `filter.*` | 五大过滤器原生库直连 + 多成员串联循环 |
| `container.hpp` | ContainerReader 公共契约 |
| `engines.*` | libarchive 容器引擎 + 密码候选迭代 + D2 双模式回退 + 7z.dll 路由 |
| `szcom.*` | **7z.dll（IInArchive COM）适配层**：CreateObject 入口、IInStream（文件/spool 卷窗口）、卷回调（RAR 多卷）、双密码回调（打开/抽取）、逐条目抽取 |
| `volumeset.*` | 分片分组与完整性预检：字节拼接型 + `.z01+.zip` + **RAR 原生卷（.partN/.rNN）** |
| `password.*` | 分层密码链（缓存→上次成功→候选→交互）+ 安全擦除 |
| `sink.*` | 路径消毒、大小写重名、`.part` 原子落名、限额 |
| `walker.*` | 递归策略引擎（Limiter、条目级分片暂存含"旧式首卷先到"处理） |
| `main.cpp` | CLI |

## 实现要点（M1）

- **7z.dll 入口**：现代 7z.dll 只导出 `CreateObject(clsid, iid, out)`（不导出 DllGetClassObject）；
  GUID 依 SDK `Guid.txt` 体系：接口 `{23170F69-40C1-278A-0000-00yy00xx0000}`、处理器 `...1000-000110xx0000`（7z=07、rar=03、rar5=CC）。
- **PROPID 实值**：kpidIsDir=6、kpidSize=7、kpidEncrypted=15（枚举位置易错位，错位会读到 PackSize）。
- **密码双通道**：头加密在 Open 回调要密码；内容加密在 Extract 回调要密码（`ICryptoGetTextPassword` 两个回调都要实现）。
- **错误映射**：`SetOperationResult` op=9 是 `kWrongPassword`（SDK NOperationResult 枚举），与损坏明确区分。
- **RAR 多卷**：`IArchiveOpenVolumeCallback::GetStream(名字)` 按基名解析卷——文件系统级直接开文件（免 spool），条目级用 spool 卷窗口。

## 已知限制（M2 计划）

- FS 级 7z/rar 根文件仍先 spool（条目级 RAR 多卷免 spool 已做；SpoolStore 并发溢出为 M2）。
- Sink 写出为同步拷贝；写出线程池化为 M2。
- 交互密码询问已实现，自动化测试仅覆盖 `--no-prompt` 路径。
- rar4 读取由 7z.dll 覆盖，但本机 WinRAR 7.x 无法生成 rar4 测试语料。
