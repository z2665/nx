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
ROOT = os.path.dirname(HERE)
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

    # C++ 纯核心单元测试（批次 1 起：退出码推导/消毒/分片命名/单位解析等纯函数）
    unit_exe = os.path.join(os.path.dirname(os.path.abspath(NX_EXE)), "nxunit.exe")
    if os.path.exists(unit_exe):
        r = add("unit_core")
        p = subprocess.run([unit_exe], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=120)
        r.check(p.returncode == 0, "nxunit 失败:\n" + (p.stdout or "")[-800:])
    else:
        print("[run] 跳过 nxunit（未构建）")

    # 所有权模型门（TLA+/TLC，唯一权威形态——批次 4 前置验收门常驻）：
    # legacy 必违 NoLeak、weakOnly 必违 AsyncNoUseAfterDead、fixed 必须全过。
    # 硬门：缺 jar/java 直接判 FAIL（跑 tools/fetch_tla.cmd 获取），不静默跳过
    tla_jar = os.path.join(ROOT, "tools", "tla2tools.jar")
    tla_spec = os.path.join(ROOT, "tools", "ownership.tla")
    r = add("ownership_tla")
    if not os.path.exists(tla_jar):
        r.check(False, "缺 tools/tla2tools.jar——先运行 tools/fetch_tla.cmd（或手动下载放入 tools/）")
    else:
        ok = True
        notes = []
        for variant, expect in (("legacy", False), ("weakOnly", False), ("fixed", True)):
            cfg = os.path.join(ROOT, "tools", f"ownership_{variant}.cfg")
            try:
                p = subprocess.run(["java", "-jar", tla_jar, "-nowarning", "-config", cfg,
                                    tla_spec], capture_output=True, text=True,
                                   encoding="utf-8", errors="replace", timeout=300,
                                   cwd=os.path.join(ROOT, "tools"))
                clean = p.returncode == 0
                detail = (p.stdout or "")[-300:]
            except FileNotFoundError:
                r.check(False, "缺 java（TLC 运行时）——安装 JDK 11+ 后重试")
                ok = False
                break
            except subprocess.TimeoutExpired:
                clean, detail = False, "TLC 超时"
            if clean != expect:
                ok = False
                notes.append(f"{variant}: returncode={p.returncode}（期望{'通过' if expect else '违例'}）\n{detail}")
        if notes:
            r.check(False, "TLA+/TLC 门结论与预期不符:\n" + "\n".join(notes))

    # 硬门（批次 6 遗留收口）：BoundedQueue abandon 协议（roadmap §7.4）——
    # DeadRelease（dead ⇒ 双侧无驻留）/ParkedSanity（驻留=while 前提纪律）。
    # 校准反例：closeNoWake 违 ParkedSanity、abandonNoWake 违 DeadRelease
    # （两个校准变体必须违例——证明模型能抓住它存在所要防的 bug 类）
    bq_spec = os.path.join(ROOT, "tools", "boundedqueue.tla")
    r = add("boundedqueue_tla")
    if not os.path.exists(tla_jar):
        r.check(False, "缺 tools/tla2tools.jar——先运行 tools/fetch_tla.cmd（或手动下载放入 tools/）")
    else:
        ok = True
        notes = []
        for variant, expect in (("closenowake", False), ("abandonnowake", False), ("fixed", True)):
            cfg = os.path.join(ROOT, "tools", f"boundedqueue_{variant}.cfg")
            try:
                p = subprocess.run(["java", "-jar", tla_jar, "-nowarning", "-config", cfg,
                                    bq_spec], capture_output=True, text=True,
                                   encoding="utf-8", errors="replace", timeout=300,
                                   cwd=os.path.join(ROOT, "tools"))
                clean = p.returncode == 0
                detail = (p.stdout or "")[-300:]
            except FileNotFoundError:
                r.check(False, "缺 java（TLC 运行时）——安装 JDK 11+ 后重试")
                ok = False
                break
            except subprocess.TimeoutExpired:
                clean, detail = False, "TLC 超时"
            if clean != expect:
                ok = False
                notes.append(f"{variant}: returncode={p.returncode}（期望{'通过' if expect else '违例'}）\n{detail}")
        if notes:
            r.check(False, "BoundedQueue TLC 门结论与预期不符:\n" + "\n".join(notes))

    # M1：zip/7z/rar 三主流格式（带密码参数）
    for case, entry, args, want in [
        ("rar5_plain", "data.rar", [], 0),
        ("rar_solid", "solid.rar", [], 0),
        ("rar_encrypted", "vault.rar", ["-p", "RarPw@2024", "--no-prompt"], 0),
        ("rar_multivol", "mv.part1.rar", [], 0),
        ("rar_entry_level_volumes", "outer.zip", [], 0),
        ("7z_encrypted", "sealed.7z", ["-p", "7zPw@2024", "--no-prompt"], 0),
        ("7z_solid_many", "solidmany.7z", ["-p", "SolidPw@2024", "--no-prompt"], 0),
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

    # solid 批量抽取性能回归（szcom materializeBatch）：逐条目单独 Extract 会对
    # solid 块逐文件从头重解码（O(N²)）——600 文件语料分钟级；批量后秒级
    for r0 in [r for r in results if r.name == "7z_solid_many"]:
        r0.check(r0.dt < 60, f"solid 批量抽取过慢（{r0.dt:.0f}s，疑似逐条目回退）")

    # M0 常规：正确解出 + 零中间
    for case, entry in [
        ("plain_zip", "plain.zip"),
        ("three_layer", "data.tar.gz"),
        ("split_zip", "data.zip.001"),
        ("split_entry_level", "outer.tar.gz"),
        ("multimember_gz", "data.tar.gz"),
        ("7z_nested", "outer.tar.gz"),
        ("zip_slip", "slip.zip"),
        ("long_path", "longpath.zip"),
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

    # 隐写（DEVELOP 待办 #1）：--stego 模式解出根文件内藏压缩包（根文件本体不落盘）
    for case, entry, args, want in [
        ("stego_mp4_zip", "video.mp4", ["--stego"], 0),
        ("stego_jpg_zip", "photo.jpg", ["--stego"], 0),
        ("stego_mp4_mdat0_zip", "clip.mp4", ["--stego"], 0),
        ("stego_mp4_rar", "movie.mp4", ["--stego"], 0),
        ("stego_mp4_7z", "film.mp4", ["--stego", "-p", "7zPw@2024", "--no-prompt"], 0),
        ("stego_disguise", "trap.mp4", ["--stego"], 0),
        ("stego_disguise_pw", "vault.mp4", ["--stego", "-p", "StegoPw@2024", "--no-prompt"], 0),
        ("stego_zip64_shadow", "ghost.mp4", ["--stego"], 0),
        ("nested_zip_stored", "outer.zip", [], 0),
    ]:
        d = os.path.join(CASES, case)
        if not os.path.isdir(d):
            print(f"[run] 跳过缺失用例 {case}")
            continue
        r = add(case)
        run_extract_and_compare(r, case, find_input(d, entry), args, want)

    # 隐写未命中：干净 MP4 → exit 0 且零输出（"查了没有"不算失败）
    d = os.path.join(CASES, "stego_none")
    if os.path.isdir(d):
        r = add("stego_none")
        out = fresh_out("stego_none")
        tmp = fresh_tmp("stego_none")
        code, _o, stderr, _t = run_nx(["extract", find_input(d, "clean.mp4"), "-O", out,
                                       "--temp-dir", tmp, "--stego"])
        r.check(code == 0, f"退出码 {code}（期望 0）stderr={stderr.strip()[:200]}")
        r.check(not os.listdir(out), f"不应有输出: {os.listdir(out)[:5]}")
        r.check(not os.listdir(tmp), "临时目录残留")
    else:
        print("[run] 跳过 stego_none")

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

    # 深度炸弹：退出码 3（12 层 zip 链 > 默认 10，决策 D-1）
    d = os.path.join(CASES, "depth_bomb")
    if os.path.isdir(d):
        r = add("depth_bomb")
        out = fresh_out("depth_bomb")
        tmp = fresh_tmp("depth_bomb")
        code, _o, stderr, _t = run_nx(["extract", find_input(d, "bomb.zip"), "-O", out,
                                       "--temp-dir", tmp])
        r.check(code == 3, f"深度炸弹退出码 {code}（期望 3）: {stderr.strip()[:200]}")

    # 过滤器链深度炸弹（决策 D-1）：30 层嵌套 gzip > 链上限 10 → 退出码 3
    # （修复前过滤器分支同 depth 无限递归 = DoS 面）
    d = os.path.join(CASES, "filter_depth_bomb")
    if os.path.isdir(d):
        r = add("filter_depth_bomb")
        out = fresh_out("filter_depth_bomb")
        tmp = fresh_tmp("filter_depth_bomb")
        code, _o, stderr, _t = run_nx(["extract", find_input(d, "bomb.gz"), "-O", out,
                                       "--temp-dir", tmp, "--no-prompt"])
        r.check(code == 3, f"过滤器链炸弹退出码 {code}（期望 3）: {stderr.strip()[:200]}")

    # 密码失败重试（D6 触发族 1 回归）：嵌套加密 zip + 前置目录条目 + 首轮错密码
    # → 重试解开（修复前该形态的失败读取器因 replayQ_ 自引用环永不析构）
    d = os.path.join(CASES, "pw_retry_nested")
    if os.path.isdir(d):
        r = add("pw_retry_nested")
        run_extract_and_compare(
            r, "pw_retry_nested", find_input(d, "outer.tar.gz"),
            ["-p", "WrongPw@1", "-p", "RetryPw@2026", "--no-prompt"], 0)
        # 无密码 + 非交互 → 密码耗尽 → 退出码 2（同触发族，失败即弃置）
        out = fresh_out("pw_retry_nested_nopw")
        tmp = fresh_tmp("pw_retry_nested_nopw")
        code, _o, _e, _t = run_nx(["extract", find_input(d, "outer.tar.gz"), "-O", out,
                                   "--temp-dir", tmp, "--no-prompt"])
        r.check(code == 2, f"无密码场景退出码 {code}（期望 2）")

    # 密码缓存键语义（批次 2 / 领域 #2）：不同父容器下的同名分片组——逻辑路径键
    # 区分兄弟分支（修复前 a 组耗尽候选污染共享游标，b 组假性耗尽 → 0 文件）
    d = os.path.join(CASES, "sibling_pw_cache")
    if os.path.isdir(d):
        r = add("sibling_pw_cache")
        run_extract_and_compare(
            r, "sibling_pw_cache", find_input(d, "outer.tar.gz"),
            ["-p", "SibB@2026", "--no-prompt"], 2)   # a 组缺密码 → 2；b 组必须解开

    # 非法数值参数（D2）：from_chars 全量校验 → 退出码 64（原 std::terminate）
    r = add("arg_validation")
    d = os.path.join(CASES, "plain_zip")
    for bad in (["--depth", "abc"], ["--depth", "12x"], ["--depth", "99999999999999999999"],
                ["--max-ratio", "1x"]):
        code, _o, _e, _t = run_nx(["extract", find_input(d, "plain.zip"), "-O",
                                   os.path.join(WORK, "arg_validation_out")] + bad)
        r.check(code == 64, f"{' '.join(bad)} 退出码 {code}（期望 64）")

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

    # ---- 文件名编码（§3.2 EFS 位缺失：CP932/GBK 原始字节名）----
    for case in ("cp_names_jp", "cp_names_cn"):
        d = os.path.join(CASES, case)
        if os.path.isdir(d):
            r = add(case)
            run_extract_and_compare(r, case, find_input(d, "cpnames.zip"), ["--no-prompt"], 0)

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

    # 右键菜单 install/remove（ExtendedSubCommandsKey 级联断言）
    # 副作用纪律：测试真实装卸 HKCU 菜单——先保存现场，结束还原用户原安装（含原 exe 路径），
    # 否则每次跑测试都会吃掉用户已装的右键菜单
    r = add("context_menu")
    PARENT = "Software\\Classes\\*\\shell\\nxExtract"
    CASCADE = "Software\\Classes\\nx.ContextMenu"
    had_menu = False
    saved_cmd = None
    try:
        import winreg

        def _reg_read(sub, value=""):
            try:
                k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, sub)
                v = winreg.QueryValueEx(k, value)[0]
                winreg.CloseKey(k)
                return v
            except OSError:
                return None

        saved_cmd = _reg_read(CASCADE + "\\shell\\nx.here\\command")
        had_menu = saved_cmd is not None
        code, _o, _e, _t = run_nx(["menu", "install"])
        r.check(code == 0, "menu install 退出码")
        verb = _reg_read(PARENT, "MUIVerb")
        ext = _reg_read(PARENT, "ExtendedSubCommandsKey")
        r.check(verb == "nx 解压", f"MUIVerb={verb}")
        r.check(ext == "nx.ContextMenu", f"ExtendedSubCommandsKey={ext}")
        for leaf, want, arg in (("nx.here", "解压到当前目录", "extract-here"),
                                ("nx.into", "解压到指定目录…", "extract-into"),
                                ("nx.stego", "解压隐写压缩包…", "extract-stego")):
            sub = CASCADE + "\\shell\\" + leaf
            title = _reg_read(sub)
            cmdLine = _reg_read(sub + "\\command")
            r.check(title == want, f"{leaf} 标题={title}")
            r.check(cmdLine and arg in cmdLine and "%1" in cmdLine,
                    f"{leaf} 命令行={str(cmdLine)[:80]}")
    except ImportError:
        r.check(False, "winreg 不可用")
    finally:
        code, _o, _e, _t = run_nx(["menu", "remove"])
        r.check(code == 0, "menu remove 退出码")
    try:
        import winreg
        for key in (PARENT, CASCADE + "\\shell\\nx.here"):
            winreg.OpenKey(winreg.HKEY_CURRENT_USER, key)
            r.check(False, f"menu remove 后键仍存在: {key}")
    except FileNotFoundError:
        pass
    if had_menu and saved_cmd:
        orig = saved_cmd.split('"')[1] if saved_cmd.startswith('"') else saved_cmd.split()[0]
        if os.path.exists(orig):
            subprocess.run([orig, "menu", "install"], capture_output=True)
            print(f"[run] 已还原用户右键菜单（原 exe: {orig}）")
        else:
            print(f"[run] 注意：原菜单 exe 不存在，未还原: {orig}")

    # 日志：文件存在 + 内容含运行头与汇总；5MiB 截断；密码红线过滤（D1）
    r = add("logging")
    exeDir = os.path.dirname(os.path.abspath(NX_EXE))
    logf = os.path.join(exeDir, "nx.log")
    r.check(os.path.exists(logf), "nx.log 未生成")
    if os.path.exists(logf):
        content = open(logf, encoding="utf-8", errors="replace").read()
        r.check("==== nx" in content, "日志缺少运行头")
        r.check('"tool"' in content, "日志缺少 report JSON（默认 reporter）")
        # D1：命令行密码绝不入日志（项目第一安全纪律）
        marker = "NxLogRedactProbe42"
        out = fresh_out("log_redact")
        run_nx(["extract", find_input(os.path.join(CASES, "plain_zip"), "plain.zip"),
                "-O", out, "-p", marker, "--no-prompt"])
        content = open(logf, encoding="utf-8", errors="replace").read()
        r.check(marker not in content, "命令行密码明文泄漏到 nx.log（D1）")
        r.check("-p ***" in content, "密码脱敏标记 *** 未出现（D1）")
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

    # 所有权 AST 强闭包审计（批次 5，硬门）：esft 类的成员强闭包含自身 = 类型级
    # 自引用环。链路 = clang-cl ast-dump → 边表（shared_ptr→派生展开）→ F* 验证
    # closure_check.exe。校准标准（2d20794 恰报 LaSeqReader 零误报）见 --calibrate
    r = add("ownership_audit")
    p = subprocess.run([sys.executable, os.path.join(HERE, "audit_ownership.py")],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=1800)
    r.check(p.returncode == 0, "所有权审计失败:\n" + (p.stdout or "")[-800:])

    # clang-tidy 基线门（批次 6，硬门）：roadmap §7.3 推荐集四检查，零警告基线
    # （存量已清零或 NOLINT 附理由）。缺 clang-tidy 组件直接 FAIL——门就是门
    r = add("tidy_check")
    p = subprocess.run([sys.executable, os.path.join(HERE, "tidy_check.py")],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=1800)
    r.check(p.returncode == 0, "clang-tidy 基线失败:\n" + (p.stdout or "")[-800:])

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
