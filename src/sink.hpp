// sink.hpp：安全落盘（设计 D4/D6/D8：线程池写出、路径消毒、原子写、限额、sha256 校验）
#pragma once
#include "bytesource.hpp"
#include "pipes.hpp"
#include "session.hpp"
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace nx {

// 段消毒：控制字符、ADS 冒号、保留名、尾部点/空格、..、绝对路径
std::string sanitize_segment(const std::string& seg);

// 逐段消毒相对路径（纯：dedupe 的消毒半部与重名登记状态分离；
// '/' 分隔逐段、空段跳过、全空回退 "_"）
std::string sanitize_rel(const std::string& rel);

struct VerifiedFile {
    std::string rel;
    uint64_t bytes;
    std::string sha256;   // --verify sha256 时有效
};

class Sink {
public:
    Sink(std::wstring outRoot, const Options& opt, Stats& stats, bool dryRun);
    ~Sink();

    // rel：逻辑相对路径（'/' 分隔，多段）；返回实际使用的相对路径（重名可能改写）。
    // src 所有权转入（异步任务需持有）。independent=true 时提交线程池异步写
    // （源为独立 spool/文件支撑，不受迭代前进影响，D4）。
    std::string emitFile(const std::string& rel, std::shared_ptr<ByteSource> src,
                         uint64_t expectedSize, int displayDepth, bool independent = false);
    void emitDir(const std::string& rel, int displayDepth);

    // 收尾：等待全部异步写完成；硬错误抛出（经 stats.abortFlag 全局生效）
    void waitAll();

    bool verifyEnabled() const { return verify_; }
    const std::vector<VerifiedFile>& verified() const { return verified_; }

private:
    std::string dedupe(const std::string& rel);
    void note(const std::string& line, int depth);
    void writeOne(const std::string& r, const std::wstring& finalPath, uint64_t expectedSize,
                  ByteSource& src, int displayDepth);
    void recordHardError(const std::string& msg);

    std::wstring outRoot_;
    const Options& opt_;
    Stats& stats_;
    bool dryRun_;
    bool verify_;
    std::unique_ptr<ThreadPool> pool_;   // D4：写出线程池 min(8, cores/2)
    bool waited_ = false;                // S5：waitAll 已调用（INV-SINK 析构前置）
    std::mutex m_;                       // 重名登记/校验列表（firstHardError 的锁在 HardErrorSlot 内）
    std::set<std::string> usedLower_;    // 大小写不敏感重名登记
    // 重名候选号游标（红队 m1：原「从 2 线性重试」对 N 同名条目 O(N²)——
    // 5000 条实测 13.2s。登记永不撤销 → 游标单调前进即正确）
    std::unordered_map<std::string, int> nextDedupe_;
    std::vector<VerifiedFile> verified_;
};

} // namespace nx
