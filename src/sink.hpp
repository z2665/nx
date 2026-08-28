// sink.hpp：安全落盘（设计 D4/D6/D8：线程池写出、路径消毒、原子写、限额、sha256 校验）
#pragma once
#include "bytesource.hpp"
#include "pipes.hpp"
#include "session.hpp"
#include <map>
#include <mutex>
#include <set>

namespace nx {

// 段消毒：控制字符、ADS 冒号、保留名、尾部点/空格、..、绝对路径
std::string sanitize_segment(const std::string& seg);

struct VerifiedFile {
    std::string rel;
    uint64_t bytes;
    std::string sha256;   // --verify sha256 时有效
};

class Sink {
public:
    Sink(std::wstring outRoot, const Options& opt, Stats& stats, bool dryRun);

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
    std::mutex m_;
    std::mutex errM_;
    std::set<std::string> usedLower_;    // 大小写不敏感重名登记
    std::vector<VerifiedFile> verified_;
};

} // namespace nx
