#include "spool.hpp"
#include "diag.hpp"
#include <algorithm>
#include <cstring>

namespace nx {

SpoolBuffer::SpoolBuffer(size_t ramCap, const std::wstring& tempDir, uint64_t diskCap)
    : ramCap_(ramCap ? ramCap : (64 << 20)), diskCap_(diskCap), tempDir_(tempDir) {
    diag::track_spool(this);   // S1 哨兵：活性登记
}

SpoolBuffer::~SpoolBuffer() {
    diag::untrack_spool(this);
    // 溢出卷 tmp_ 析构自动关句柄（DELETE_ON_CLOSE：内核即删）
}

// 溢出目录水位（D6"解压中周期复查"的 spool 落点）：余量 < 待写 + 64MiB 即
// 熔断——把 WriteFile 中途 disk-full 的原始错误换成可读的 LimitError（exit 3）。
// 查询失败（目录不可解析等）跳过本次检查（与 sink 的水位语义一致）。
void SpoolBuffer::checkDiskWater(uint64_t pending) {
    if (waterDir_.empty()) {
        if (!tempDir_.empty()) {
            waterDir_ = tempDir_;
        } else {
            wchar_t buf[MAX_PATH];
            UINT n = GetTempPathW(MAX_PATH, buf);
            if (n == 0 || n >= MAX_PATH) return;   // 解析失败：跳过检查
            waterDir_.assign(buf, n);
        }
    }
    ULARGE_INTEGER fb{};
    if (GetDiskFreeSpaceExW(waterDir_.c_str(), &fb, nullptr, nullptr) &&
        fb.QuadPart < pending + (64ull << 20))
        throw LimitError("磁盘空间不足（spool 溢出写中止，剩余 " +
                         format_size(fb.QuadPart) + "）");
}

void SpoolBuffer::flushToTemp() {
    if (overflowed_) return;
    // 转换点先过两道闸（M6）：RAM 全量即将整段落盘
    if (total_ > diskCap_)
        throw LimitError("spool 磁盘溢出超过上限 " + format_size(diskCap_) +
                         "（--max-bytes 可调）");
    checkDiskWater(ram_.size());
    // 唯一临时文件工厂（res/，P2 圈禁）：FILE_FLAG_DELETE_ON_CLOSE——句柄一关
    // （正常析构/异常退出/进程被杀）OS 即删；独占句柄天然满足"唯一持有者"；
    // spool 读经同一句柄（不按路径重开），无冲突。
    // （真实案例：15GB spool 在成功运行结束后残留——对象级泄漏/强杀时 DeleteFileW
    // 永远没机会执行，DELETE_ON_CLOSE 把清理责任交给内核）
    tmp_ = res::TempFile::create(tempDir_);
    overflowed_ = true;
    // RAM 全量落盘（分块 ≤16MiB：ram_ 可达 8GiB，一次 cast DWORD 会截断——
    // 真实案例 8GiB 恰为 2×4GiB，截断成 0 后 WriteFile 成功写入 0 字节，
    // 表现为"写临时文件失败: 操作成功完成 (Win32 0)"）
    size_t off = 0;
    while (off < ram_.size()) {
        size_t want = std::min<size_t>(ram_.size() - off, 16u << 20);
        DWORD wrote = 0;
        if (!WriteFile(tmp_.handle.get(), ram_.data() + off, static_cast<DWORD>(want), &wrote,
                       nullptr)
            || wrote == 0)
            throw Error("写临时文件失败: " + wide_to_utf8(win32_last_error_text()));
        off += wrote;
    }
    ram_.clear();
    ram_.shrink_to_fit();
}

void SpoolBuffer::append(std::span<const byte> p) {
    if (finished_) throw Error("spool 已结束，不能再写入");
    if (p.empty()) return;
    if (!overflowed_) {
        if (ram_.size() + p.size() <= ramCap_) {
            ram_.insert(ram_.end(), p.begin(), p.end());
            total_ += p.size();
            return;
        }
        flushToTemp();
    }
    // 溢出路径：直接写文件
    std::lock_guard<std::mutex> lk(ioM_);
    size_t off = 0;
    while (off < p.size()) {
        DWORD wrote = 0;
        if (!WriteFile(tmp_.handle.get(), p.data() + off, static_cast<DWORD>(p.size() - off),
                       &wrote, nullptr)
            || wrote == 0)
            throw Error("写临时文件失败: " + wide_to_utf8(win32_last_error_text()));
        off += wrote;
    }
    total_ += p.size();
    // M6：溢出总量熔断 + 水位周期复查（每次溢出写都查——块粒度下开销可忽略，
    // GetDiskFreeSpaceExW 仅元数据查询）。上限只约溢出相位：RAM 相位由 ramCap
    // 自界（--spool-ram），磁盘才是无上界的那个
    if (total_ > diskCap_)
        throw LimitError("spool 磁盘溢出超过上限 " + format_size(diskCap_) +
                         "（--max-bytes 可调）");
    checkDiskWater(p.size());
}

void SpoolBuffer::finish() {
    finished_ = true;
}

size_t SpoolBuffer::readAt(uint64_t pos, std::span<byte> buf) {
    if (!finished_) throw Error("spool 尚未结束，不能读取");
    if (pos >= total_) return 0;
    uint64_t avail = total_ - pos;
    size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), avail));
    if (!overflowed_) {
        std::memcpy(buf.data(), ram_.data() + pos, n);
        return n;
    }
    std::lock_guard<std::mutex> lk(ioM_);
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(pos);
    if (!SetFilePointerEx(tmp_.handle.get(), li, nullptr, FILE_BEGIN))
        throw Error("临时文件定位失败: " + wide_to_utf8(win32_last_error_text()));
    size_t got = 0;
    while (got < n) {
        DWORD r = 0;
        if (!ReadFile(tmp_.handle.get(), buf.data() + got, static_cast<DWORD>(n - got), &r,
                      nullptr) || r == 0)
            throw Error("读临时文件失败: " + wide_to_utf8(win32_last_error_text()));
        got += r;
    }
    return got;
}

} // namespace nx
