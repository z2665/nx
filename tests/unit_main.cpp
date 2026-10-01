// unit_main.cpp：纯核心单元测试（批次 1 起的 C++ 单测壳 nxunit）
// 运行：build\nxunit.exe；全部通过退出 0，失败打印用例位置并退出 1。
// 纪律（roadmap P5）：只测纯函数——IO/线程/GUI 归属性测试（run_tests.py）与 fuzz。
#include "outcome.hpp"
#include "util.hpp"
#include "format.hpp"
#include "detect.hpp"
#include "layer.hpp"
#include "report.hpp"
#include "sink.hpp"
#include "stego.hpp"
#include "volumeset.hpp"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace nx;

namespace {

int g_fail = 0;
int g_total = 0;

void check(bool cond, const char* what, int line) {
    ++g_total;
    if (!cond) {
        ++g_fail;
        std::printf("[FAIL] line %d: %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(a, b) check((a) == (b), #a " == " #b, __LINE__)

// ---- derive_exit_code（领域 #4）：退出码契约全分支 ----
void test_derive_exit_code() {
    CHECK_EQ(derive_exit_code({}), 0);
    OutcomeFlags f;

    f = {};
    f.cancelled = true;
    f.limitTripped = true;   // 取消优先于一切
    CHECK_EQ(derive_exit_code(f), 2);

    f = {};
    f.limitTripped = true;
    f.passwordFail = true;
    CHECK_EQ(derive_exit_code(f), 3);

    f = {};
    f.passwordFail = true;
    CHECK_EQ(derive_exit_code(f), 2);

    f = {};
    f.missingVolume = true;
    f.corrupt = true;
    CHECK_EQ(derive_exit_code(f), 4);

    f = {};
    f.corrupt = true;
    CHECK_EQ(derive_exit_code(f), 1);

    f = {};
    f.branchesFailed = 3;
    CHECK_EQ(derive_exit_code(f), 1);

    f = {};
    f.branchesFailed = 0;
    CHECK_EQ(derive_exit_code(f), 0);
}

// ---- ascii_lower：locale 无关，CJK 原样 ----
void test_ascii_lower() {
    CHECK_EQ(ascii_lower("Hello.ZIP"), std::string("hello.zip"));
    CHECK_EQ(ascii_lower(""), std::string(""));
    // 中文（UTF-8 字节 >= 0x80）不得被改动
    CHECK_EQ(ascii_lower("\xe4\xb8\xad\x41"), std::string("\xe4\xb8\xad\x61"));
    CHECK_EQ(ascii_lower("[X@Y]"), std::string("[x@y]"));   // 非字母不动
}

// ---- strip_filter_suffixes：裸过滤器命名 ----
void test_strip_filter_suffixes() {
    CHECK_EQ(strip_filter_suffixes("a.tar.gz"), std::string("a"));
    CHECK_EQ(strip_filter_suffixes("A.TAR.GZ"), std::string("A"));   // 大小写不敏感
    CHECK_EQ(strip_filter_suffixes("b.tgz"), std::string("b"));
    CHECK_EQ(strip_filter_suffixes("c.Z"), std::string("c"));        // L4：小写登记后 .Z 可匹配
    CHECK_EQ(strip_filter_suffixes("d.exe"), std::string("d.exe~")); // 无可剥离 → 加 ~
    CHECK_EQ(strip_filter_suffixes(".gz"), std::string(".gz~"));     // pos==0 无基本名不算后缀
}

// ---- sanitize_segment：路径消毒规则（D6）----
void test_sanitize_segment() {
    CHECK_EQ(sanitize_segment("normal.txt"), std::string("normal.txt"));
    CHECK_EQ(sanitize_segment("con"), std::string("_con"));          // 保留名
    CHECK_EQ(sanitize_segment("COM1.dat"), std::string("_COM1.dat"));
    CHECK_EQ(sanitize_segment("a:b"), std::string("a_b"));           // ADS 冒号
    CHECK_EQ(sanitize_segment("x*?<>|\""), std::string("x______"));  // 非法字符
    CHECK_EQ(sanitize_segment("\x01z"), std::string("_z"));          // 控制字符
    CHECK_EQ(sanitize_segment("."), std::string("_"));               // 相对路径特例
    CHECK_EQ(sanitize_segment(".."), std::string("__"));
    CHECK_EQ(sanitize_segment("tail. "), std::string("tail"));       // 尾部点/空格
    CHECK_EQ(sanitize_segment("..."), std::string("_"));             // 全剥空 → 占位
}

// ---- match_split_name：分片命名识别（§3.3）----
void test_match_split_name() {
    CHECK(!match_split_name("plain.zip"));
    CHECK(!match_split_name("noext"));

    auto a = match_split_name("data.zip.001");
    CHECK(a && a->key == "data.zip" && a->index == 1 && !a->zspan && !a->nativeRar);

    auto z = match_split_name("pack.z01");
    CHECK(z && z->key == "pack" && z->index == 1 && z->zspan);

    auto p = match_split_name("mv.part2.rar");
    CHECK(p && p->key == "mv" && p->index == 2 && p->nativeRar);

    auto r = match_split_name("old.r00");
    CHECK(r && r->key == "old" && r->index == 2 && r->nativeOldRar);

    CHECK(!match_split_name("x.0"));        // 位数不足
    CHECK(!match_split_name("x.z0"));
    CHECK(!match_split_name("x.zip.abcdef"));  // 非纯数字
}

// ---- format_info 表（领域 #1）：类属/名称查询与表完整性 ----
void test_format_info() {
    CHECK_EQ(classify(Format::Gzip), FormatClass::Filter);
    CHECK_EQ(classify(Format::Brotli), FormatClass::Filter);
    CHECK_EQ(classify(Format::Tar), FormatClass::SeqContainer);
    CHECK_EQ(classify(Format::Zip), FormatClass::TailContainer);
    CHECK_EQ(classify(Format::SevenZip), FormatClass::RandContainer);
    CHECK_EQ(classify(Format::Wim), FormatClass::RandContainer);
    CHECK_EQ(classify(Format::Unknown), FormatClass::None);
    CHECK(std::string_view(format_name(Format::CompressZ)) == "compress(.Z)");
    CHECK(std::string_view(format_name(Format::Unknown)) == "unknown");
    for (size_t i = 0; i < std::size(kFormatTable); ++i)
        for (size_t j = i + 1; j < std::size(kFormatTable); ++j)
            CHECK(kFormatTable[i].fmt != kFormatTable[j].fmt);
}

// ---- detect_from_bytes（纯核心）：魔数/结构校验/SFX ----
void test_detect_from_bytes() {
    auto det = [](std::vector<byte> v, std::optional<uint64_t> hint = std::nullopt) {
        return detect_from_bytes(std::span<const byte>(v.data(), v.size()), hint);
    };
    // gzip：魔数 + FLG 高 3 位须为 0
    CHECK(det({byte(0x1f), byte(0x8b), byte(8), byte(0)}).fmt == Format::Gzip);
    CHECK(det({byte(0x1f), byte(0x8b), byte(8), byte(0xE0)}).fmt == Format::Unknown);
    // zip 本地头：合法字段 vs 异常（方法越界仍判 zip 交引擎报错）
    std::vector<byte> lfh(34, 0);
    lfh[0] = 'P'; lfh[1] = 'K'; lfh[2] = 3; lfh[3] = 4;
    lfh[26] = 4;   // nlen=4
    CHECK(det(lfh).fmt == Format::Zip);
    CHECK(det(lfh).detail == "stored");   // method=0
    std::vector<byte> badLfh = lfh;
    badLfh[8] = 200;   // method > 99 → 字段异常分支（仍 Zip）
    CHECK(det(badLfh).fmt == Format::Zip);
    // SFX：前缀 + zip 头 → sfxOffset 结构化记录，display 合成展示
    std::vector<byte> sfx(1000, 0);
    std::memcpy(sfx.data() + 500, lfh.data(), lfh.size());
    Detection d = det(sfx);
    CHECK(d.fmt == Format::Zip);
    CHECK(d.sfxOffset && *d.sfxOffset == 500);
    CHECK(d.display() == std::string("zip SFX@+500"));   // SFX 分支不带 stored（与原行为一致）
    // 7z SFX
    std::vector<byte> sfx7(64, 0);
    const byte m7[] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
    std::memcpy(sfx7.data() + 10, m7, 6);
    d = det(sfx7);
    CHECK(d.fmt == Format::SevenZip && d.sfxOffset && *d.sfxOffset == 10);
    CHECK(d.display() == std::string("7z SFX@+10"));
    // 空窗口/垃圾 → Unknown，display 为 "unknown"
    CHECK(det({}).fmt == Format::Unknown);
    CHECK(det({1, 2, 3, 4, 5}).fmt == Format::Unknown);
    CHECK(det({1, 2, 3, 4, 5}).display() == std::string("unknown"));
    // iso：偏移 0x8001 的 CD001，且 sizeHint 门槛生效
    std::vector<byte> iso(0x8006 + 16, 0);
    std::memcpy(iso.data() + 0x8001, "CD001", 5);
    CHECK(det(iso).fmt == Format::Iso);
    CHECK(det(iso, 100).fmt == Format::Unknown);   // 提示小于 0x8006 → 不判 iso
}

// ---- eocd_from_window / parse_atom_header（stego 纯核心）----
namespace {
// 构造 EOCD（小端字段）：cdSize/cdOff/commentLen
std::vector<byte> make_eocd(uint32_t cdSize, uint32_t cdOff, uint16_t commentLen) {
    std::vector<byte> e(22);
    e[0] = 'P'; e[1] = 'K'; e[2] = 5; e[3] = 6;
    for (int i = 0; i < 4; ++i) {
        e[12 + i] = static_cast<byte>((cdSize >> (8 * i)) & 0xFF);
        e[16 + i] = static_cast<byte>((cdOff >> (8 * i)) & 0xFF);
    }
    e[20] = static_cast<byte>(commentLen & 0xFF);
    e[21] = static_cast<byte>(commentLen >> 8);
    return e;
}
} // namespace

void test_eocd_from_window() {
    using namespace stego;
    // 布局：[100B 前缀][CD 40B @100][EOCD @140]，cdOff=100、cdSize=40 → base=0
    std::vector<byte> win(100);
    win.insert(win.end(), 40, byte(0));              // CD 区
    auto e = make_eocd(40, 100, 0);
    win.insert(win.end(), e.begin(), e.end());       // EOCD 收尾
    uint64_t fileSize = win.size();
    auto r = eocd_from_window(win, 0, fileSize);
    CHECK(r.kind == stego::EocdScanResult::Kind::Candidate);
    CHECK(r.cand.base == 0);
    CHECK(r.cand.len == 140 + 22);
    CHECK(r.cand.cdVerifyPos == 140 - 40);

    // 带注释 + 窗口起点偏移：base = (winStart + EOCD 窗内偏移) - cdOff - cdSize
    std::vector<byte> w2(4997, byte(0));
    auto e2 = make_eocd(30, 70, 3);
    w2.insert(w2.end(), e2.begin(), e2.end());
    w2.insert(w2.end(), 3, byte(0x41));              // 3 字节注释
    r = eocd_from_window(w2, 900, 900 + w2.size());
    CHECK(r.kind == stego::EocdScanResult::Kind::Candidate);
    CHECK(r.cand.base == 900 + 4997 - 100);

    // zip64 影子标记 → Distrust（整体放弃）
    auto e3 = make_eocd(40, 0xFFFFFFFFu, 0);
    std::vector<byte> w3(e3.begin(), e3.end());
    r = eocd_from_window(w3, 0, w3.size());
    CHECK(r.kind == stego::EocdScanResult::Kind::Distrust);

    // 空目录 → Distrust；注释越界 → 跳过；CD 越过 EOCD → 跳过；无 EOCD → None
    auto e4 = make_eocd(0, 0, 0);
    std::vector<byte> w4(e4.begin(), e4.end());
    r = eocd_from_window(w4, 0, w4.size());
    CHECK(r.kind == stego::EocdScanResult::Kind::Distrust);

    auto e5 = make_eocd(10, 0, 100);                 // 注释 100B 但只留 0B
    std::vector<byte> w5(e5.begin(), e5.end());
    CHECK(eocd_from_window(w5, 0, w5.size()).kind == stego::EocdScanResult::Kind::None);

    auto e6 = make_eocd(500, 0, 0);                  // cdEnd > eocdAbs
    std::vector<byte> w6(e6.begin(), e6.end());
    CHECK(eocd_from_window(w6, 0, w6.size()).kind == stego::EocdScanResult::Kind::None);

    std::vector<byte> w7(64, byte(0));
    CHECK(eocd_from_window(w7, 0, w7.size()).kind == stego::EocdScanResult::Kind::None);
    CHECK(eocd_from_window({}, 0, 0).kind == stego::EocdScanResult::Kind::None);
}

void test_parse_atom_header() {
    byte hdr[8] = {0, 0, 0, 16, 'm', 'd', 'a', 't'};   // size=16
    auto a = stego::parse_atom_header(hdr);
    CHECK(!a.toEof && a.hdrSize == 8 && a.atomSize == 16);
    byte hdr0[8] = {0, 0, 0, 0, 'm', 'd', 'a', 't'};   // size=0 → 延伸到 EOF
    a = stego::parse_atom_header(hdr0);
    CHECK(a.toEof);
    byte hdr1[8] = {0, 0, 0, 1, 'm', 'd', 'a', 't'};   // size=1 → 扩展长度
    byte ext[8] = {0, 0, 0, 0, 0, 0, 1, 0};           // be64 = 256
    a = stego::parse_atom_header(hdr1, ext);
    CHECK(!a.toEof && a.hdrSize == 16 && a.atomSize == 256);
    a = stego::parse_atom_header(hdr1);                 // ext 缺失 → 非法（atomSize < hdrSize）
    CHECK(!a.toEof && a.atomSize < a.hdrSize);
}

// ---- render_report（纯渲染）：结构、转义、verify 分支 ----
void test_render_report() {
    ReportData d;
    d.tool = "nx";
    d.inputs = {R"(D:\a "quoted".zip)", "plain.zip"};
    d.files = 3;
    d.bytes = 1024;
    d.inputBytes = 512;
    d.containers = 2;
    d.durationMs = 42;
    std::string j = render_report(d);
    CHECK(j.find("\"tool\": \"nx\"") != std::string::npos);
    // 引号与反斜杠转义
    CHECK(j.find(R"(D:\\a \"quoted\".zip)") != std::string::npos);
    CHECK(j.find("\"files\": 3") != std::string::npos);
    CHECK(j.find("\"inputBytes\": 512") != std::string::npos);
    CHECK(j.find("\"verify\": null") != std::string::npos);   // 未启用 verify
    CHECK(j.find("password") == std::string::npos ||   // 无密码值（prompts 计数字段除外）
          j.find("\"passwordPrompts\"") != std::string::npos);

    d.verifyEnabled = true;
    d.verified = {VerifiedFile{"a\nb.txt", 10, "cafe"}};
    j = render_report(d);
    CHECK(j.find("\"verify\": [") != std::string::npos);
    CHECK(j.find(R"("a\nb.txt")") != std::string::npos);      // 控制字符 \u000a 转义
    CHECK(j.find("\"sha256\": \"cafe\"") != std::string::npos);
}

// ---- sanitize_rel：多段路径消毒（dedupe 的纯半部）----
void test_sanitize_rel() {
    CHECK_EQ(sanitize_rel("dir/file.txt"), std::string("dir/file.txt"));
    CHECK_EQ(sanitize_rel("a/../b"), std::string("a/__/b"));      // .. 段改写
    CHECK_EQ(sanitize_rel("con/x.txt"), std::string("_con/x.txt"));
    CHECK_EQ(sanitize_rel("/abs/path"), std::string("abs/path")); // 空首段跳过
    CHECK_EQ(sanitize_rel("//x//y"), std::string("x/y"));
    CHECK_EQ(sanitize_rel(""), std::string("_"));                 // 全空回退
    CHECK_EQ(sanitize_rel("d./t.. .txt"), std::string("d/t.. .txt"));  // 非整段 .. 不改写
}

// ---- LayerCtx / 层身份派生（领域 #2，批次 2）----
void test_layer_ctx() {
    // join_logical：空父 = 根；逐层延伸
    CHECK_EQ(join_logical("", "outer.zip"), std::string("outer.zip"));
    CHECK_EQ(join_logical("outer.tar.gz/a.tar.gz", "data.zip"),
             std::string("outer.tar.gz/a.tar.gz/data.zip"));

    // 兄弟分支键区分（批次 2 语义修正的核心性质）
    LayerId a = make_layer_id("o.tar.gz/a.tar.gz", "data.zip", 3, "zip");
    LayerId b = make_layer_id("o.tar.gz/b.tar.gz", "data.zip", 3, "zip");
    CHECK(a.key != b.key);
    CHECK_EQ(a.display, std::string("第 3 层 data.zip (zip)"));   // 展示同形（键异）

    // 根层：空逻辑父 → key = origin
    LayerId root = make_layer_id("", "root.zip", 1, "zip");
    CHECK_EQ(root.key, std::string("root.zip"));

    // forEntry：换 origin/链、回新容器段（链清零、非过滤器）；sub/logical 不动
    LayerCtx parent;
    parent.sub = "o.tar.gz";
    parent.origin = "o.tar.gz";
    parent.chain = "gzip → tar";
    parent.logical = "o.tar.gz";
    parent.depth = 1;
    parent.filterChain = 2;
    parent.throughFilter = true;
    LayerCtx entry = parent.forEntry("dir1/a.zip", "gzip → tar → zip");
    CHECK_EQ(entry.origin, std::string("dir1/a.zip"));
    CHECK_EQ(entry.chain, std::string("gzip → tar → zip"));
    CHECK_EQ(entry.filterChain, 0);
    CHECK(!entry.throughFilter);
    CHECK_EQ(entry.sub, std::string("o.tar.gz"));      // 输出前缀随父
    CHECK_EQ(entry.logical, std::string("o.tar.gz"));  // 逻辑路径随父（容器打开时才延伸）
    CHECK_EQ(entry.depth, 1);

    // forFilter：链+1、深度/逻辑路径不变
    LayerCtx flt = parent.forFilter("gzip → gzip", 3);
    CHECK_EQ(flt.filterChain, 3);
    CHECK_EQ(flt.depth, 1);
    CHECK_EQ(flt.logical, std::string("o.tar.gz"));
    CHECK(flt.throughFilter);
}

// ---- parse_size：单位与拒绝 ----
void test_parse_size() {
    CHECK_EQ(parse_size("1048576"), uint64_t(1) << 20);
    CHECK_EQ(parse_size("1M"), uint64_t(1) << 20);
    CHECK_EQ(parse_size("2GiB"), uint64_t(2) << 30);
    CHECK_EQ(parse_size("1T"), uint64_t(1) << 40);
    bool threw = false;
    try {
        parse_size("-1M");
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        parse_size("1x");
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
}

} // namespace

int main() {
    test_derive_exit_code();
    test_ascii_lower();
    test_strip_filter_suffixes();
    test_sanitize_segment();
    test_match_split_name();
    test_format_info();
    test_detect_from_bytes();
    test_eocd_from_window();
    test_parse_atom_header();
    test_render_report();
    test_sanitize_rel();
    test_layer_ctx();
    test_parse_size();
    std::printf("nxunit: %d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
