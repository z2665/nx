# fuzz 回归种子（git 跟踪）

历史 fuzz 崩溃工件，`tests/fuzz_run.py` 的 `populate_seeds()` 每次运行前从本目录
回灌进 `tests/fuzz-corpus-seeds/`（该目录 gitignored 且每次重建，只有这里是持久层）。
复现：`python tests/fuzz_run.py --rerun tests/fuzz-regression/<文件>`。

## 工件登记

| 文件 | 捕获时间 | 缺陷 | 修复 |
|---|---|---|---|
| crash-dd3680f56a2be96b8b25b5a7e3bc10453f2bfd59 | 2026-10-02 批次 5 迁移开发期（未提交中间态） | szcom tryOpen：com_ptr 把归档对象释放推迟到迭代末尾，Open 失败后先清 mainStream_ → 7z.dll 随后 Release 悬空流指针（ASan heap-use-after-free，711 次迭代即抓） | arc.reset() 显式先于 mainStream_.reset()（szcom.cpp tryOpen 顺序契约注释） |

新增工件：fuzz 崩溃修复后将工件移入本目录并登记上行（-commit message 之外的
持久事实源）。
