# nx — 流式嵌套压缩包解压工具

一个命令、或一次右键，把**任意深度嵌套**的压缩包直接解到最终文件树——中间任何一层的压缩包本身不落盘。
支持嵌套异密码、各种分片、MP4 隐写压缩包。Windows 10+，单文件 exe（约 5.5 MB，绿色免安装）。

```
data.zip.001/.002/.003        ← 分片，自动拼接
  └─ data.zip                 ← 外层 zip（可加密）
      ├─ readme.txt
      └─ inner.7z             ← 嵌套 7z（可以是另一个密码）
          └─ bundle.tar.gz    ← 再嵌套两层
              └─ photos.zip ...
```

现有工具没有能同时做到这几点的：7-Zip/WinRAR 不递归嵌套；手工管道（`tar -xOf … | tar -xf -`）不认分片、不支持多层密码；同类工具（unblob 等）每层先落盘再解，磁盘和 I/O 翻倍。nx 的定位就是补上这块**策略层**：分片虚拟拼接 + 按内容递归识别 + 流水线并发 + 分层密码 + 安全边界。

## 功能一览

- **按内容识别每一层**：magic + 结构校验，不依赖扩展名（嵌套内容往往没有扩展名）
- **流水线并发，中间文件零落盘**：各级解压并发执行，总耗时 ≈ 最慢一级；链式场景实测快于"手工两遍解压"约 15–25%，中间磁盘占用恒为 0（NVMe/256MiB 语料基准，`python tests/bench.py` 可复现；数字随磁盘与层级构成浮动，单层场景无流水线收益、基本持平）
- **分片全支持**：`.001/.002…` 拼接型、`.z01+.zip` 顺序陷阱、RAR 原生分卷（新旧命名）、压缩包内散落的"条目级分片"
- **分层异密码**：每层独立密码链（缓存 → 上次成功 → `-p` 候选列表 → GUI 弹窗询问）；密码可经 `--password-file` 提供，绝不写日志
- **MP4 隐写解压**：`extract-stego` 解出视频/文件尾部内藏的 zip/7z/rar（根文件本体不落盘）
- **安全边界默认开启**：递归深度 / 累计输出 / 压缩比熔断（防 zip 炸弹）、路径消毒（zip-slip / 保留名 / ADS / 超长路径）、`.part` 临时名原子落盘
- **格式**：zip（含 SFX、ZipCrypto/WinZip-AES、CP932/GBK 文件名修复）/ 7z（AES、头加密、分卷、solid）/ rar（rar5、加密、多卷）/ tar / gzip / bzip2 / xz / zstd / lz4 / .Z / iso / cab …

## 安装

1. 下载便携包（`dist\nx\` 或 Release），解压到任意目录。
2. （推荐）把 **7z.dll** 放在 nx.exe 同目录（也可安装 7-Zip 或让它留在 PATH）——负责 7z 全特性与 RAR；缺失时自动回退内置引擎（RAR 多卷除外）。
3. （可选）安装右键菜单：在 nx.exe 所在目录运行 `nx menu install`（仅当前用户，免管理员；`nx menu remove` 卸载）。

## 使用

**右键菜单**（安装后）：对任意压缩包右键 →「nx 解压」→

- 解压到当前目录
- 解压到指定目录…（弹窗询问前缀，默认 = 文件名去扩展名，WinRAR 惯例）
- 解压隐写压缩包…（对 MP4 等视频文件）

**命令行**：

```cmd
nx extract data.zip.001 -O out\                & :: 全自动：分片→嵌套→并行落盘
nx extract outer.zip -O out\ --depth 3         & :: 限制递归深度（默认 10）
nx tree outer.tar.gz                           & :: 只看嵌套结构，不落盘
nx extract x.7z.001 -p pw1 -p pw2 --no-prompt  & :: 脚本场景：仅候选列表
nx extract x.zip --max-bytes 100G --keep-going & :: 限额 + 损坏隔离继续
nx extract x.zip --verify sha256 --report r.json
nx extract-stego video.mp4 -O out\             & :: 解出视频内藏的压缩包
```

常用选项：`-O` 输出目录｜`-p`/`--password-file` 密码｜`--no-prompt` 非交互｜`--depth`/`--max-bytes`/`--max-ratio` 安全限额｜`--keep-going` 损坏继续｜`--verify sha256` 输出校验｜`--report r.json` 机器可读报告（不含密码）。完整列表：`nx --help`。

**GUI 行为**：从资源管理器启动（或 `--gui`）时自动弹密码窗（每层一窗）、进度窗（真百分比 = 根输入消耗比；取消 = 安静退出并清理半成品）、完成提示。终端/管道下行为与普通 CLI 完全一致。每次运行的日志追加在 nx.exe 旁的 `nx.log`（超 5 MiB 自动截断，密码红线过滤）。

**退出码**：`0` 成功｜`1` 部分失败｜`2` 密码缺失或耗尽（含用户取消）｜`3` 超限熔断｜`4` 缺分片｜`64` 用法错误。

## 已知限制

| # | 问题 | 说明 |
|---|---|---|
| 1 | 嵌套隐写检测 | 隐写扫描仅根文件层（需 seek）；压缩包内的 MP4 不查 |
| 2 | 条目级分片连续到达 | 分片组成员须连续到达，非成员条目到达即封组 |
| 3 | RAR4/旧命名卷 | 读取由 7z.dll 覆盖，但缺少测试验证（WinRAR 7.x 无法生成 rar4 语料；rar5 族经 LFS 冻结语料全环境测试） |
| 4 | tar 内符号链接 | 跳过并告警，不落盘 |
| 5 | Win11 新版右键菜单 | 经典级联菜单完整可用；新版菜单（MSIX）未启用 |

## 许可

本项目代码采用 **MIT 许可**（[LICENSE](LICENSE)）。
nx.exe 静态链接 libarchive（BSD）、zlib（Zlib）、bzip2（BSD）、liblzma（公有领域）、zstd 与 lz4（BSD/GPLv2 双许可）。
7z.dll 为 7-Zip（Igor Pavlov，LGPL），独立 DLL 按需加载、可选放置——发行物组件许可见 [LICENSE-distro.txt](LICENSE-distro.txt)。

## 文档

- **开发者**：[DEVELOP.md](DEVELOP.md) —— 构建（VS 2026 + vcpkg）、测试体系（65 项门 + fuzz + TLA+/F\* 验证）、架构分层、重构记录
- **设计文档**：[nested-extractor-design.md](nested-extractor-design.md) —— 领域模型、架构、所有权与生命周期、全部关键决策
- **AI 协作代理**：[AGENTS.md](AGENTS.md) —— 工作区纪律（所有权纪律 P1-P6、安全红线、决策速查、踩坑记录），自动化代理修改本仓库前必读
