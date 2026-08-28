// container.hpp：容器读取器公共契约（engines 与 szcom 共用）
#pragma once
#include "bytesource.hpp"
#include "format.hpp"
#include "password.hpp"
#include <memory>
#include <string>

namespace nx {

struct ContainerEntry {
    std::string name;
    uint64_t size = UINT64_MAX;      // 解压后大小；未知 = UINT64_MAX
    bool isDir = false;
    bool isSymlink = false;
    std::string symlinkTarget;
    std::shared_ptr<ByteSource> data;   // 顺序条目流：调用下一次 next() 前有效
};

class ContainerReader {
public:
    virtual ~ContainerReader() = default;
    // false = 迭代结束。CorruptError（数据坏）/ PasswordExhausted（密码耗尽）经异常抛出。
    virtual bool next(ContainerEntry& out) = 0;
};

struct EngineOptions {
    size_t spoolRam = 64 << 20;
    std::wstring tempDir;
};

} // namespace nx
