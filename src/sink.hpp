// sink.hpp：安全落盘（设计 D6：路径消毒 / 重名 / 原子写 / 限额）
#pragma once
#include "bytesource.hpp"
#include "session.hpp"
#include <mutex>
#include <set>

namespace nx {

// 段消毒：控制字符、ADS 冒号、保留名、尾部点/空格、..、绝对路径
std::string sanitize_segment(const std::string& seg);

class Sink {
public:
    Sink(std::wstring outRoot, const Options& opt, Stats& stats, bool dryRun);

    // rel：逻辑相对路径（可能多段，'/' 分隔）。返回实际使用的相对路径（可能因重名改写）。
    // 写 src 全部内容；expectedSize 已知时校验；限额熔断抛 LimitError；CRC/截断抛 CorruptError。
    std::string emitFile(const std::string& rel, ByteSource& src, uint64_t expectedSize,
                         int displayDepth);
    void emitDir(const std::string& rel, int displayDepth);

private:
    std::string dedupe(const std::string& rel);
    void note(const std::string& line, int depth);

    std::wstring outRoot_;
    const Options& opt_;
    Stats& stats_;
    bool dryRun_;
    std::mutex m_;
    std::set<std::string> usedLower_;   // 大小写不敏感重名登记
};

} // namespace nx
