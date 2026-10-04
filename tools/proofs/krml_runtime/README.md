# krml_runtime/ — KaRaMeL C 运行时最小集（vendored）

`closure_check.exe`（AST 闭包检查器的判定内核）链接的 F\*/KaRaMeL 运行时片段，
自本机 F\* 发行包（`tools/fstar/`，gitignored、用户自配）原样收编——使这门验证
在任何环境（含 CI）可从仓库内容复现，不依赖本地 F\* 安装。

- `include/krml/`：krml 伞形头与 `krml/internal/*`（类型/兼容/目标层）
- `karamel/`：KaRaMeL 抽取的模块头与实现（closure_check 实际链接 `fstar_int32.c`；
  目录名避开仓库 `.gitignore` 的 `dist/` 规则）
- `c/prims.c`：Primitives 运行时实现

出处：F\* 发行包（https://fstar-lang.org/ ，KaRaMeL 运行时，Apache License 2.0）。
仅原样拷贝、未修改；升级 F\* 时随 `tools/proofs/Closure.fst` 重抽取一并对齐。
`krml_glue/` 的前置 krmllib.h（compat 前置 + include_next 接本目录真伞）与
`krml_out/`（Closure.fst 的 KaRaMeL 产物）不变，见上级目录。
