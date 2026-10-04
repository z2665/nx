#!/usr/bin/env python3
"""nx 所有权审计：AST 强闭包检查器 + P2 圈禁 grep 检查。

链路：clang-cl -Xclang -ast-dump=json 逐 TU 抽取"类→成员强边"表 →
（shared_ptr/unique_ptr 目标展开到全部传递派生类——环常经基类静态类型达成，
 replayQ_ 事故即如此）→ F* 验证 + KaRaMeL 抽取的 closure_check.exe 判定
 esft 类的成员强闭包是否含自身（= 类型级自引用环）。
圈禁口径（写死五名单，评审 M-5）：CloseHandle / DeleteFileW / RegCloseKey /
 FreeLibrary / ->Release() 不得出现在 src/res/ 之外（匹配"名字+调用括号"，
 剥注释防误报；IUnknown::Release override 是 COM 接口实现，不属手工释放）。
 已知边界：点调用 x.Release()、经函数指针/宏的间接调用不匹配（当前代码无此形态）；
 fclose/LocalFree/CoTaskMemFree/archive_read_free 暂不在圈禁名单（暂不圈禁）。
 每次 audit 先跑圈禁门自检（正/负样本注入），防 regex 失效后空转通过。

用法：
  python tests/audit_ownership.py                 # 审计 HEAD src/，违例退出 1
  python tests/audit_ownership.py --src <dir>     # 审计指定源目录（校准用）
  python tests/audit_ownership.py --calibrate     # 校准模式：对 2d20794 须恰报
                                                  # LaSeqReader 一处零误报
                                                  # （圈禁检查不参与校准——其标准是
                                                  #   HEAD 零违规，基线时代尚未圈禁）
依赖：clang-cl（VS "C++ Clang tools for Windows" 组件）、vcvars、
      tools/proofs/closure_check.exe（tools/build_closure_kernel.cmd 产出）
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VSROOT = r"C:\Program Files\Microsoft Visual Studio\18\Community"
CLANG = os.path.join(VSROOT, "VC", "Tools", "Llvm", "x64", "bin", "clang-cl.exe")
KERNEL = os.path.join(ROOT, "tools", "proofs", "closure_check.exe")
CALIBRATE_REF = "2d20794"

STD_SMART = ("shared_ptr", "unique_ptr")
STD_CONTAINERS = ("deque", "vector", "list", "set", "unordered_set",
                  "map", "unordered_map", "optional")
SKIP_PREFIX = ("std::function", "std::atomic", "std::jthread", "std::thread",
               "std::basic_string", "std::string", "std::wstring",
               "std::mutex", "std::condition_variable", "std::exception_ptr")


def last_component(qt: str) -> str:
    """'nx::`anonymous namespace'::LaEntrySource' → 'LaEntrySource'（含模板尾巴清洗）"""
    qt = qt.strip()
    qt = re.sub(r"^class +|^struct +", "", qt)
    return qt.split("::")[-1].strip()


def run_clang_dump(src_dir: str, tu: str) -> dict | None:
    """经临时 .cmd（vcvars + clang-cl）产出 ast-dump=json；clang 退出码非 0 即失败。
    直接拼 cmd /c 字符串的引号/重定向解析与脚本文件不一致——实测只有脚本文件路径可靠。
    评审 M-2 加固（原实现两缺陷曾致验收门非确定）：
    ①输出/脚本按 TU+pid 独立命名且先删旧——原固定名 nx_ast.json 会在 clang 失败时
      读到上一 TU 的陈旧 dump（张冠李戴伪成功）；
    ②clang 退出码透传（原 `exit /b 0` 吞错）；
    ③补真实构建的编译定义（与 CMakeLists 一致）——基线时代 18/52 TU 因 std::min 被
      windows.h 的 min 宏污染而解析失败即此因。"""
    inc = os.path.join(ROOT, "build", "vcpkg_installed", "x64-windows-static", "include")
    stem = re.sub(r"[^A-Za-z0-9_]", "_", os.path.splitext(tu)[0])
    out = os.path.join(tempfile.gettempdir(), f"nx_ast_{stem}_{os.getpid()}.json")
    script = os.path.join(tempfile.gettempdir(), f"nx_ast_dump_{stem}_{os.getpid()}.cmd")
    for p in (out, script):
        try:
            os.remove(p)
        except OSError:
            pass
    body = ("\n".join([
        "@echo off",
        f'call "{VSROOT}\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul 2>&1',
        f'"{CLANG}" /nologo /std:c++latest /EHsc -fsyntax-only -Wno-everything ^',
        f'  /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /DCRT_SECURE_NO_WARNINGS ^',
        f'  -I"{src_dir}" -I"{inc}" "{os.path.join(src_dir, tu)}" ^',
        f'  -Xclang -ast-dump=json > "{out}" 2>nul',
        "exit /b %ERRORLEVEL%",
    ]) + "\r\n").replace("\n", "\r\n").replace("\r\r", "\r")
    with open(script, "w", encoding="utf-8", newline="") as f:
        f.write(body)
    r = subprocess.run(["cmd", "/c", script], capture_output=True, timeout=300)
    try:
        if r.returncode != 0:
            return None
        with open(out, encoding="utf-8", errors="replace") as f:
            if f.read(1) != "{":
                return None
            f.seek(0)
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None
    finally:
        for p in (out, script):
            try:
                os.remove(p)
            except OSError:
                pass


def walk_ast(node, records, esft):
    """收集 CXXRecordDecl：name / bases / fields（含模板实例化与嵌套类）"""
    if isinstance(node, dict):
        kind = node.get("kind")
        if kind in ("CXXRecordDecl", "ClassTemplateSpecializationDecl",
                    "ClassTemplatePartialSpecializationDecl") and "name" in node:
            name = node["name"]
            bases = [last_component(b.get("type", {}).get("qualType", ""))
                     for b in node.get("bases", [])]
            fields = []
            for inner in node.get("inner", []):
                if inner.get("kind") == "FieldDecl":
                    qt = inner.get("type", {}).get("qualType", "")
                    fields.append(qt)
            if name not in records:  # 跨 TU 首见为准，重复解析合并字段
                records[name] = {"bases": bases, "fields": fields}
            else:
                for qt in fields:
                    if qt not in records[name]["fields"]:
                        records[name]["fields"].append(qt)
            if any("enable_shared_from_this" in b for b in bases):
                esft.add(name)
        for inner in node.get("inner", []):
            walk_ast(inner, records, esft)
    elif isinstance(node, list):
        for item in node:
            walk_ast(item, records, esft)


def split_top_args(s: str):
    """'A<B,C>, D' → ['A<B,C>', 'D']（顶层逗号分割）"""
    out, depth, cur = [], 0, []
    for ch in s:
        if ch in "<(":
            depth += 1
        elif ch in ">)":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        out.append("".join(cur).strip())
    return [a for a in out if a]


def template_of(qt: str):
    """'const std::shared_ptr<X>' / 'std::deque<Y>' → (head, inner) 或 None"""
    m = re.search(r"([A-Za-z_][\w:]*)\s*<(.*)>\s*(?:[*&]|const)?\s*$", qt.strip())
    if not m:
        return None
    return m.group(1), m.group(2)


def plain_type_target(qt: str, records):
    """顶层裸记录类型（按值组合 = 强边）：裸指针/引用/函数/数组不算"""
    qt = qt.strip()
    if not qt or "(" in qt or "[" in qt or qt.endswith("*") or qt.endswith("&"):
        return None
    if any(qt.replace("const ", "").strip().startswith(p) for p in SKIP_PREFIX):
        return None
    name = last_component(qt)
    return name if name in records else None


def strong_targets(qt: str, records, smart_only=False):
    """字段类型 → (目标集合)；目标 ∈ {plain, smart}（smart 需派生展开）"""
    plains, smarts = set(), set()
    qt = qt.strip()

    def visit(t: str, top: bool):
        t = t.strip()
        if any(x in t for x in SKIP_PREFIX if t.startswith(x)) or "(" in t:
            return
        if "weak_ptr" in t:
            return  # 非拥有
        tpl = template_of(t)
        if tpl:
            head, inner = tpl
            head_last = last_component(head)
            if head_last in STD_SMART:
                for a in split_top_args(inner):
                    nm = last_component(a)
                    if nm in records or not smart_only:
                        smarts.add(last_component(a))
            elif head_last in STD_CONTAINERS:
                args = split_top_args(inner)
                vals = args[1:] if head_last in ("map", "unordered_map") else args
                for a in vals:
                    visit(a, top=False)
            return
        # 裸记录类型：容器元素（元素所有权）与顶层按值组合同为强边；
        # 裸指针/引用（借用）不边——top 与否的差别只是指针语法出现的位置
        if t.endswith("*") or t.endswith("&"):
            return
        p = plain_type_target(t, records)
        if p:
            plains.add(p)

    visit(qt, top=True)
    return plains, smarts


def derived_closure(name: str, children: dict) -> set:
    out, frontier = set(), [name]
    while frontier:
        c = frontier.pop()
        for d in children.get(c, ()):
            if d not in out:
                out.add(d)
                frontier.append(d)
    return out


BASE_RE = re.compile(
    r"(?:^|\n)\s*(?:class|struct)\s+([A-Za-z_]\w*)\s*(?::\s*((?:[^{;]|\n)*?))?\s*\{")


def textual_bases(src_dir: str) -> dict:
    """clang ast-dump=json 不序列化基类——从源码文本补'类→基类'表。
    本仓库基类均为简单名/单层模板（enable_shared_from_this<T>），文本法足够。"""
    bases = {}
    for f in os.listdir(src_dir):
        if not f.endswith((".cpp", ".hpp")):
            continue
        text = open(os.path.join(src_dir, f), encoding="utf-8", errors="replace").read()
        for m in BASE_RE.finditer(text):
            name, bs = m.group(1), m.group(2)
            if not bs:
                continue
            out = []
            for b in bs.split(","):
                b = re.sub(r"\bvirtual\b|\bpublic\b|\bprivate\b|\bprotected\b", "", b)
                b = last_component(b.split("<")[0] if "enable_shared_from_this" in b else b)
                b = b.strip()
                if b and b not in out:
                    out.append(b)
            bases.setdefault(name, out)
    return bases


def build_graph(src_dir: str):
    records, esft = {}, set()
    tus = sorted(f for f in os.listdir(src_dir) if f.endswith((".cpp", ".hpp")))
    failed = []
    for tu in tus:
        ast = run_clang_dump(src_dir, tu)
        if ast is None:
            failed.append(tu)
            continue
        walk_ast(ast, records, esft)
    if failed:
        # 评审 M-2：fail-loud。原"跳过——校准零误报前提下可接受"曾让覆盖面逐次
        # 抖动、同一 HEAD 判定在 52/53↔53/53 翻转——TU 不在图上即盲区，不可跳过
        print(f"[audit] ❌ {len(failed)} 个 TU AST 解析失败（盲区即失败，不跳过）:")
        for t in failed:
            print(f"        {t}")
        return None, None, None, None
    if not tus:
        print("[audit] 无 TU——src 目录异常")
        return None, None, None, None

    children = {}
    text_bases = textual_bases(src_dir)
    for name, info in records.items():
        for b in info["bases"] + text_bases.get(name, []):
            children.setdefault(b, []).append(name)

    edges = set()
    for name, info in records.items():
        for qt in info["fields"]:
            plains, smarts = strong_targets(qt, records)
            for p in plains:
                if p in records:
                    edges.add((name, p))
            for s in smarts:
                s = last_component(s)
                if s in records:
                    edges.add((name, s))
                    for d in derived_closure(s, children):
                        if d in records:
                            edges.add((name, d))
    return records, esft, edges, children


# ---- P2 圈禁：释放调用只允许出现在 src/res/ ----
# 匹配"名字+调用括号"（注释中无括号的提及不误报；先剥注释再扫以稳妥）
RELEASE_CALL = re.compile(
    r"\b(?:CloseHandle|DeleteFileW|RegCloseKey|FreeLibrary)\s*\(|->Release\s*\(")

COMMENT_LINE = re.compile(r"//[^\n]*")
COMMENT_BLOCK = re.compile(r"/\*.*?\*/", re.DOTALL)


def quarantine_violations(src_dir):
    """src_dir 下 res/ 之外的手工释放调用清单（空 = 圈禁达成）。"""
    res_dir = os.path.normcase(os.path.join(src_dir, "res"))
    bad = []
    for dirpath, _dirs, files in os.walk(src_dir):
        for f in sorted(files):
            if not f.endswith((".cpp", ".hpp")):
                continue
            p = os.path.join(dirpath, f)
            if os.path.normcase(os.path.dirname(p)) == res_dir:
                continue   # 圈禁区本体
            with open(p, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
            text = COMMENT_BLOCK.sub(" ", COMMENT_LINE.sub(" ", text))
            for ln, line in enumerate(text.splitlines(), 1):
                if RELEASE_CALL.search(line):
                    try:
                        rel = os.path.relpath(p, ROOT)
                    except ValueError:
                        rel = p   # 跨盘（--src 在另一盘）——退回绝对路径
                    bad.append(f"{rel}:{ln}: {line.strip()}")
    return bad


def selftest_quarantine() -> bool:
    """圈禁门自检（评审 M-3）：正/负样本注入临时目录——regex 被改坏（空转）或
    误报即 False。每次 audit 运行前执行（秒级），防门失效后 53/53 照绿的假通过。"""
    import shutil
    d = tempfile.mkdtemp(prefix="nx_qa_selftest_")
    try:
        os.makedirs(os.path.join(d, "res"))
        cases = {
            "a.cpp": "void f() { HANDLE h; CloseHandle(h); }\n",
            "b.cpp": "struct S { long Release(); }; void g(S* p) { p->Release(); }\n",
            "res/c.cpp": ("void h() { CloseHandle(nullptr); DeleteFileW(nullptr);"
                          " RegCloseKey(nullptr); FreeLibrary(nullptr); }\n"),
            "d.cpp": "// 提及 CloseHandle 与 DeleteFileW 但非调用\nvoid i() {}\n",
        }
        for rel, text in cases.items():
            with open(os.path.join(d, rel), "w", encoding="utf-8") as fh:
                fh.write(text)
        bad = quarantine_violations(d)
        # 路径含盘符冒号——从右侧切 ":行号: 内容"
        hit_files = {b.rsplit(":", 2)[0] for b in bad}
        if len(bad) != 2 or not any("CloseHandle" in b for b in bad) \
                or not any("Release" in b for b in bad) \
                or not hit_files == {os.path.join(d, "a.cpp"), os.path.join(d, "b.cpp")}:
            print(f"[audit] ❌ 圈禁门自检失败（检出 {bad!r}——期望恰 a.cpp/b.cpp 各一处）")
            return False
        print("[audit] ✅ 圈禁门自检：正/负样本检出与豁免全部正确")
        return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def run_kernel(nodes, edges, queries):
    """closure_check.exe：CSR 边表 + 查询 → 每查询 0/1"""
    if not os.path.exists(KERNEL):
        print(f"[audit] 缺 {KERNEL}——先跑 tools/build_closure_kernel.cmd")
        sys.exit(2)
    ids = {n: i for i, n in enumerate(sorted(nodes))}
    lines = [f"{len(nodes)} {len(edges)}"]
    for u, v in sorted(edges):
        lines.append(f"{ids[u]} {ids[v]}")
    lines.append(str(len(queries)))
    lines.extend(str(ids[q]) for q in queries)
    p = subprocess.run([KERNEL], input="\n".join(lines) + "\n",
                       capture_output=True, text=True, timeout=120)
    out = [int(x) for x in p.stdout.split()]
    if len(out) != len(queries):
        print(f"[audit] 内核输出异常: {p.stdout!r} {p.stderr!r}")
        sys.exit(2)
    return dict(zip(queries, out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=os.path.join(ROOT, "src"))
    ap.add_argument("--calibrate", action="store_true",
                    help="对 git 基线 2d20794 校准：恰报 LaSeqReader 一处零误报")
    args = ap.parse_args()

    if args.calibrate:
        worktree = os.path.join(ROOT, "tmp", "cal_2d20794")
        subprocess.run(["git", "worktree", "remove", "--force", worktree],
                       capture_output=True)
        r = subprocess.run(["git", "worktree", "add", "--detach", worktree,
                            CALIBRATE_REF], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"[audit] worktree 失败: {r.stderr}")
            return 2
        try:
            return audit(os.path.join(worktree, "src"), expect={"LaSeqReader"},
                         quarantine=False)
        finally:
            subprocess.run(["git", "worktree", "remove", "--force", worktree],
                           capture_output=True)
    return audit(args.src, expect=set(), quarantine=True)


def audit(src_dir, expect, quarantine=True):
    rc = 0
    if not selftest_quarantine():
        return 1
    if quarantine:
        bad = quarantine_violations(src_dir)
        if bad:
            print(f"[audit] ❌ P2 圈禁违例（释放调用越出 src/res/）× {len(bad)}:")
            for b in bad:
                print(f"        {b}")
            rc = 1
        else:
            print("[audit] ✅ P2 圈禁：res/ 之外零释放调用")
    records, esft, edges, children = build_graph(src_dir)
    if records is None:
        return 2
    print(f"[audit] 记录 {len(records)} · esft {sorted(esft)} · 强边 {len(edges)}")
    verdicts = run_kernel(set(records) | {u for u, _ in edges} | {v for _, v in edges},
                          edges, sorted(esft))
    hits = sorted(n for n, v in verdicts.items() if v)
    print(f"[audit] 自环命中: {hits or '无'}")
    if set(hits) == expect:
        if expect:
            print(f"[audit] ✅ 校准达成：恰报 {sorted(expect)}，零误报")
        else:
            print("[audit] ✅ 零违规")
        return rc
    if expect:
        print(f"[audit] ❌ 校准失败：期望恰报 {sorted(expect)}，实得 {hits}")
    else:
        print(f"[audit] ❌ 发现类型级自引用环: {hits}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
