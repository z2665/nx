#!/usr/bin/env python3
"""nx M0 属性测试运行器（设计 §9.2/§9.3/§9.4）。

对每个用例：运行 nx → 对比输出树 sha256 与 expected.json → 断言退出码。
退出码契约：0 成功 | 1 部分失败 | 2 密码 | 3 超限 | 4 缺分片。
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(HERE, "cases")
NX_EXE = os.environ.get("NX_EXE", os.path.join(HERE, "..", "build", "nx.exe"))
WORK = os.path.join(HERE, "work")


def run_nx(args, timeout=300):
    t0 = time.time()
    p = subprocess.run([NX_EXE] + args, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=timeout,
                       env={**os.environ, "NX_PROMPT_TEST": "1"})
    return p.returncode, p.stdout, p.stderr, time.time() - t0


def hash_tree(root: str) -> dict:
    out = {}
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root).replace("\\", "/")
            h = hashlib.sha256()
            with open(full, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
            out[rel] = h.hexdigest()
    return out


def case_inputs(case_dir):
    return sorted(f for f in os.listdir(case_dir)
                  if f != "expected.json" and not f.startswith("_"))


class Result:
    def __init__(self, name):
        self.name = name
        self.ok = True
        self.notes = []

    def check(self, cond, msg):
        if not cond:
            self.ok = False
            self.notes.append(msg)
        return cond


def fresh_out(name):
    out = os.path.join(WORK, name, "out")
    if os.path.exists(os.path.join(WORK, name)):
        shutil.rmtree(os.path.join(WORK, name))
    os.makedirs(out)
    return out


def fresh_tmp(name):
    t = os.path.join(WORK, name, "tmp")
    os.makedirs(t, exist_ok=True)
    for f in os.listdir(t):
        os.remove(os.path.join(t, f))
    return t


def find_input(case_dir, want):
    for f in case_inputs(case_dir):
        if f == want:
            return os.path.join(case_dir, f)
    raise AssertionError(f"输入 {want} 不存在")


def run_extract_and_compare(r, case, input_file, extra_args, expect_code):
    case_dir = os.path.join(CASES, case)
    out = fresh_out(case)
    tmp = fresh_tmp(case)
    code, stdout, stderr, dt = run_nx(
        ["extract", input_file, "-O", out, "--temp-dir", tmp] + extra_args)
    r.check(code == expect_code,
            f"退出码 {code}（期望 {expect_code}）stderr={stderr.strip()[:400]}")
    expected = json.load(open(os.path.join(case_dir, "expected.json"), encoding="utf-8"))["files"]
    if expected:
        got = hash_tree(out)
        missing = {k: v for k, v in expected.items() if got.get(k) != v}
        extra = [k for k in got if k not in expected]
        r.check(not missing, f"内容不符/缺失: {list(missing)[:5]}")
        r.check(not extra, f"多余文件（可能中间层落盘!）: {extra[:5]}")
    # 零中间文件：临时目录不残留 + 输出无 .nxpart
    leftover = os.listdir(tmp)
    r.check(not leftover, f"临时目录残留: {leftover[:5]}")
    for dirpath, _d, filenames in os.walk(out):
        for fn in filenames:
            r.check(not fn.startswith(".nxpart"), f".part 残留: {fn}")
    r.dt = dt
    r.stdout = stdout
    r.stderr = stderr
    return r


# ---------------------------------------------------------------- 用例配置

def main():
    if not os.path.exists(NX_EXE):
        print(f"[run] 找不到 {NX_EXE}，先构建")
        return 2
    os.makedirs(WORK, exist_ok=True)
    results = []

    def add(name):
        r = Result(name)
        results.append(r)
        return r

    # M1：zip/7z/rar 三主流格式（带密码参数）
    for case, entry, args, want in [
        ("rar5_plain", "data.rar", [], 0),
        ("rar_solid", "solid.rar", [], 0),
        ("rar_encrypted", "vault.rar", ["-p", "RarPw@2024", "--no-prompt"], 0),
        ("rar_multivol", "mv.part1.rar", [], 0),
        ("rar_entry_level_volumes", "outer.zip", [], 0),
        ("7z_encrypted", "sealed.7z", ["-p", "7zPw@2024", "--no-prompt"], 0),
        ("7z_mhe", "blind.7z", ["-p", "7zPw@2024", "--no-prompt"], 0),
        ("7z_split", "sp.7z.001", [], 0),
        ("zip_sfx", "installer.exe", [], 0),
        ("triple_chain", "chain.zip", ["-p", "RarChain@2024", "-p", "7zChain@2024", "--no-prompt"], 0),
    ]:
        d = os.path.join(CASES, case)
        if not os.path.isdir(d):
            print(f"[run] 跳过缺失用例 {case}")
            continue
        r = add(case)
        run_extract_and_compare(r, case, find_input(d, entry), args, want)

    # M0 常规：正确解出 + 零中间
    for case, entry in [
        ("plain_zip", "plain.zip"),
        ("three_layer", "data.tar.gz"),
        ("split_zip", "data.zip.001"),
        ("split_entry_level", "outer.tar.gz"),
        ("multimember_gz", "data.tar.gz"),
        ("7z_nested", "outer.tar.gz"),
        ("zip_slip", "slip.zip"),
        ("bare_gz", "plain.txt.gz"),
        ("zspan", "data.zip"),
        ("mixed_filters", "mixed.tar.bz2"),
    ]:
        d = os.path.join(CASES, case)
        if not os.path.isdir(d):
            print(f"[run] 跳过缺失用例 {case}")
            continue
        r = add(case)
        run_extract_and_compare(r, case, find_input(d, entry), [], 0)

    # 两层异密码：候选顺序故意与层级相反（外层密码在后）→ 均应通过候选迭代解开
    d = os.path.join(CASES, "two_passwords")
    if os.path.isdir(d):
        r = add("two_passwords")
        run_extract_and_compare(r, "two_passwords", find_input(d, "outer.zip"),
                                ["-p", "InnerPw@2024", "-p", "OuterPw@2024", "--no-prompt"], 0)
        # 无候选 + 非交互 → 密码耗尽 → 退出码 2
        out = fresh_out("two_passwords_nopw")
        tmp = fresh_tmp("two_passwords_nopw")
        code, _o, _e, _t = run_nx(["extract", find_input(d, "outer.zip"), "-O", out,
                                   "--temp-dir", tmp, "--no-prompt"])
        r.check(code == 2, f"无密码场景退出码 {code}（期望 2）")
    else:
        print("[run] 跳过 two_passwords")

    # 坏 CRC：--keep-going → 隔离 bad.txt，good.txt 存活，退出码 1
    d = os.path.join(CASES, "bad_crc")
    if os.path.isdir(d):
        r = add("bad_crc")
        run_extract_and_compare(r, "bad_crc", find_input(d, "badcrc.zip"), ["--keep-going"], 1)

    # 深度炸弹：退出码 3
    d = os.path.join(CASES, "depth_bomb")
    if os.path.isdir(d):
        r = add("depth_bomb")
        out = fresh_out("depth_bomb")
        tmp = fresh_tmp("depth_bomb")
        code, _o, stderr, _t = run_nx(["extract", find_input(d, "bomb.zip"), "-O", out,
                                       "--temp-dir", tmp])
        r.check(code == 3, f"深度炸弹退出码 {code}（期望 3）: {stderr.strip()[:200]}")

    # 缺分片：退出码 4
    d = os.path.join(CASES, "missing_volume")
    if os.path.isdir(d):
        r = add("missing_volume")
        out = fresh_out("missing_volume")
        code, _o, stderr, _t = run_nx(["extract", find_input(d, "data.zip.001"), "-O", out])
        r.check(code == 4, f"缺分片退出码 {code}（期望 4）: {stderr.strip()[:200]}")

    # tree 干跑：结构打印、不落盘
    d = os.path.join(CASES, "three_layer")
    if os.path.isdir(d):
        r = add("tree_dryrun")
        code, stdout, _e, _t = run_nx(["tree", find_input(d, "data.tar.gz")])
        r.check(code == 0, f"tree 退出码 {code}")
        r.check("inner.zip" in stdout, "tree 输出缺少 inner.zip")

    # ---- M2 ----
    # 压缩比熔断（D6）：高膨胀 gz + --max-ratio 50 → exit 3；默认 1000 放行
    d = os.path.join(CASES, "ratio_bomb")
    if os.path.isdir(d):
        r = add("ratio_bomb")
        out = fresh_out("ratio_bomb")
        code, _o, stderr, _t = run_nx(["extract", find_input(d, "bomb.bin.gz"), "-O", out,
                                       "--max-ratio", "50", "--no-prompt"])
        r.check(code == 3, f"--max-ratio 50 退出码 {code}（期望 3）: {stderr.strip()[:150]}")
        out = fresh_out("ratio_bomb_ok")
        code, _o, _e, _t = run_nx(["extract", find_input(d, "bomb.bin.gz"), "-O", out,
                                   "--no-prompt"])
        r.check(code == 0, f"默认 ratio 下退出码 {code}（期望 0）")

    # --verify sha256 + --report（D8）：哈希与 ground truth 对比、报告结构断言
    d = os.path.join(CASES, "plain_zip")
    if os.path.isdir(d):
        import json as _json
        r = add("verify_report")
        out = fresh_out("verify_report")
        tmp = fresh_tmp("verify_report")
        rep = os.path.join(WORK, "verify_report", "r.json")
        code, _o, stderr, _t = run_nx(["extract", find_input(d, "plain.zip"), "-O", out,
                                       "--temp-dir", tmp, "--verify", "sha256",
                                       "--report", rep, "--no-prompt"])
        r.check(code == 0, f"verify+report 退出码 {code}: {stderr.strip()[:150]}")
        try:
            rep = _json.load(open(rep, encoding="utf-8"))
            for key in ("tool", "inputs", "files", "bytes", "containers", "filters",
                        "durationMs", "verify"):
                r.check(key in rep, f"报告缺少字段 {key}")
            r.check(isinstance(rep["verify"], list) and len(rep["verify"]) == 2,
                    f"verify 条目数 {len(rep['verify']) if isinstance(rep['verify'], list) else 'null'}（期望 2）")
            if isinstance(rep["verify"], list):
                for v in rep["verify"]:
                    r.check(len(v.get("sha256", "")) == 64, f"sha256 长度异常: {v}")
        except Exception as e:
            r.check(False, f"报告解析失败: {e}")
        # 哈希与 ground truth 对比
        exp = _json.load(open(os.path.join(d, "expected.json"), encoding="utf-8"))["files"]
        got = {}
        for dirpath, _dn, filenames in os.walk(out):
            for fn in filenames:
                import hashlib
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, out).replace("\\", "/")
                h = hashlib.sha256()
                with open(full, "rb") as f:
                    for c in iter(lambda: f.read(1 << 20), b""):
                        h.update(c)
                got[rel] = h.hexdigest()
        for k, v in exp.items():
            r.check(got.get(k) == v, f"{k} 哈希不符（verify 数据流正确性）")

    # ---- M3 ----
    # --no-root：条目直接落在输出目录（无根目录层）
    d = os.path.join(CASES, "three_layer")
    if os.path.isdir(d):
        r = add("no_root")
        out = fresh_out("no_root")
        code, _o, stderr, _t = run_nx(["extract", find_input(d, "data.tar.gz"), "-O", out,
                                       "--no-root", "--no-prompt"])
        r.check(code == 0, f"--no-root 退出码 {code}")
        got = hash_tree(out)
        r.check("inner.zip/a.txt" in got and "loose.txt" in got and
                not any(k.startswith("data.tar.gz/") for k in got),
                f"--no-root 目录结构异常: {sorted(got)[:4]}")

    # extract-here：解压到压缩文件所在目录（无根目录层）
    import tempfile as _tf
    d = os.path.join(CASES, "plain_zip")
    if os.path.isdir(d):
        r = add("extract_here")
        eh = _tf.mkdtemp(prefix="nx_eh_")
        try:
            shutil.copy(find_input(d, "plain.zip"), os.path.join(eh, "plain.zip"))
            code, _o, stderr, _t = run_nx(["extract-here", os.path.join(eh, "plain.zip")])
            r.check(code == 0, f"extract-here 退出码 {code}: {stderr.strip()[:150]}")
            got = hash_tree(eh)
            r.check("readme.txt" in got and "dir/a.bin" in got and
                    not any(k.startswith("plain.zip/") for k in got),
                    f"extract-here 输出结构异常: {sorted(got)[:4]}")
        finally:
            shutil.rmtree(eh, ignore_errors=True)

    # 右键菜单 install/remove（HKCU 注册表断言，平级两项）
    r = add("context_menu")
    code, _o, _e, _t = run_nx(["menu", "install"])
    r.check(code == 0, "menu install 退出码")
    MENU_KEYS = ["Software\\Classes\\*\\shell\\nxExtractHere",
                 "Software\\Classes\\*\\shell\\nxExtractInto"]
    try:
        import winreg
        for key, want in ((MENU_KEYS[0], "nx 解压到当前目录"),
                          (MENU_KEYS[1], "nx 解压到指定目录…")):
            k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, key)
            title = winreg.QueryValueEx(k, "")[0]
            c = winreg.OpenKey(winreg.HKEY_CURRENT_USER, key + "\\command")
            cmdLine = winreg.QueryValueEx(c, "")[0]
            r.check(title == want, f"菜单文字={title}（期望 {want}）")
            r.check("%1" in cmdLine, f"命令行={cmdLine[:80]}")
    except ImportError:
        r.check(False, "winreg 不可用")
    finally:
        code, _o, _e, _t = run_nx(["menu", "remove"])
        r.check(code == 0, "menu remove 退出码")
    try:
        import winreg
        for key in MENU_KEYS + ["Software\\Classes\\*\\shell\\nxExtract"]:
            winreg.OpenKey(winreg.HKEY_CURRENT_USER, key)
            r.check(False, f"menu remove 后键仍存在: {key}")
    except FileNotFoundError:
        pass

    # 日志：文件存在 + 内容含运行头与汇总；5MiB 截断
    r = add("logging")
    exeDir = os.path.dirname(os.path.abspath(NX_EXE))
    logf = os.path.join(exeDir, "nx.log")
    r.check(os.path.exists(logf), "nx.log 未生成")
    if os.path.exists(logf):
        content = open(logf, encoding="utf-8", errors="replace").read()
        r.check("==== nx" in content, "日志缺少运行头")
        r.check('"tool"' in content, "日志缺少 report JSON（默认 reporter）")
        with open(logf, "wb") as f:
            f.write(b"x" * (6 * 1024 * 1024))
        run_nx(["--help"])
        r.check(os.path.getsize(logf) < 5 * 1024 * 1024 + 200000,
                f"超 5MiB 未截断: {os.path.getsize(logf)}")

    # GUI 冒烟（独立进程跑，窗口消息自动化）
    r = add("gui_smoke")
    g = subprocess.run([sys.executable, os.path.join(HERE, "gui_smoke.py")],
                       capture_output=True, text=True, encoding="utf-8", errors="replace",
                       timeout=180)
    r.check(g.returncode == 0, "GUI 冒烟失败: " + (g.stdout or "")[-500:])

    # 汇总
    print()
    fails = 0
    for r in results:
        status = "PASS" if r.ok else "FAIL"
        dt = f" {getattr(r, 'dt', 0):.2f}s" if hasattr(r, "dt") else ""
        print(f"  [{status}] {r.name}{dt}")
        for n in r.notes:
            print(f"         - {n}")
        fails += 0 if r.ok else 1
    total = len(results)
    print(f"\n{total - fails}/{total} 通过")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
