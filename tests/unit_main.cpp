// unit_main.cpp：纯核心单元测试（批次 1 起的 C++ 单测壳 nxunit）
// 运行：build\nxunit.exe；全部通过退出 0，失败打印用例位置并退出 1。
// 纪律（roadmap P5）：只测纯函数——IO/线程/GUI 归属性测试（run_tests.py）与 fuzz。
#include "outcome.hpp"
#include "util.hpp"
#include "format.hpp"
#include "sink.hpp"
#include "volumeset.hpp"

#include <cstdio>
#include <string>

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
    test_parse_size();
    std::printf("nxunit: %d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
