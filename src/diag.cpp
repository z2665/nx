// diag.cpp：泄漏哨兵实现（S1-S5；NX_DIAG_LEAKS 时才有实体）
#include "diag.hpp"

#ifdef NX_DIAG_LEAKS

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>

namespace nx::diag {

namespace {

struct Registry {
    std::mutex m;
    std::unordered_set<const void*> spools;
    std::unordered_set<const void*> readers;
};

// 故意 new 不删：退出检查经 atexit 运行，须晚于一切静态析构顺序问题
Registry& registry() {
    static Registry* r = new Registry();
    return *r;
}

[[noreturn]] void die(const char* what) {
    std::fflush(stderr);
    std::fprintf(stderr, "[diag] 哨兵违规：%s\n", what);
    std::fflush(stderr);
    std::abort();
}

} // namespace

void track_spool(const void* self) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    r.spools.insert(self);
}

void untrack_spool(const void* self) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    r.spools.erase(self);
}

void track_reader(const void* self) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    r.readers.insert(self);
}

void untrack_reader(const void* self) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    r.readers.erase(self);
}

std::size_t live_readers() {
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    return r.readers.size();
}

void check_all_destroyed(const char* where) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    if (r.spools.empty() && r.readers.empty()) return;
    std::fflush(stderr);
    std::fprintf(stderr, "[diag] %s：泄漏残留 spool=%zu reader=%zu\n", where,
                 r.spools.size(), r.readers.size());
    die("S1/S2 活性注册表非空");
}

TryOpenGuard::TryOpenGuard() : base(live_readers()) {}

TryOpenGuard::~TryOpenGuard() {
    if (escaped) return;
    std::size_t now = live_readers();
    if (now != base) {
        std::fflush(stderr);
        std::fprintf(stderr, "[diag] try_open 失败出口泄漏读取器：进 %zu 出 %zu\n", base, now);
        die("S3 失败尝试的读取器未析构");
    }
}

void assert_true(bool cond, const char* what) {
    if (!cond) die(what);
}

void exit_check() {
    check_all_destroyed("exit");
}

} // namespace nx::diag

#endif // NX_DIAG_LEAKS
