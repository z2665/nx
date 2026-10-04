// views.hpp：seekable 三态视图（唯一实现：engines.cpp 与 szcom.cpp
// 两套逐行重复的 SeekView/SeekInput 家族合并）+ 工厂与挂表纪律类型化。
// 用途：zip 中央目录模式、7z.dll 卷回调、嵌套免 spool 直读的随机访问支撑
#pragma once
#include "bytesource.hpp"
#include "spool.hpp"
#include "res/unique_handle.hpp"
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <windows.h>

namespace nx {

// 可 seek 只读视图：RegionSource 契约的"完整视图"标记（挂锁实现的三态：
// 文件/spool/区间链）。标记的实质契约有两层——
//   ① 并发安全：read_at 可多线程并发调用（FileSeekView 持锁、spool 读经
//     SpoolBuffer::ioM_ 串行化、RegionView 转发到同样满足本契约的父视图）；
//   ② 类型筛选：open.cpp 经 dynamic_pointer_cast<SeekView> 确保免 spool 直读的
//     区间支撑是三态视图之一，而非任意 RegionSource。
// 不再重声明基类纯虚函数（原样重写一遍即噪声——两契约全在注释与类型身份上）
class SeekView : public RegionSource {};

// spool 窗口视图：[start, start+len)（len=0 到 spool 末尾）——随机访问形态；
// 同一区间的顺序流形态 = SpoolBuffer::Window（spool.hpp），两形态各服务一类消费方
class SpoolWindowView : public SeekView {
public:
    SpoolWindowView(std::shared_ptr<SpoolBuffer> s, uint64_t start, uint64_t len);
    size_t read_at(uint64_t pos, std::span<byte> buf) override;
    uint64_t size() const override { return len_; }
private:
    std::shared_ptr<SpoolBuffer> spool_;
    uint64_t start_, len_;
};

// 文件窗口视图：[base, base+length)（length=0 到 EOF）；meter 非空时计量读取
class FileSeekView : public SeekView {
public:
    explicit FileSeekView(const std::wstring& path, InputMeter* meter = nullptr,
                          uint64_t base = 0, uint64_t length = 0);
    size_t read_at(uint64_t pos, std::span<byte> buf) override;
    uint64_t size() const override { return size_; }
private:
    res::UniqueFile h_;   // P2 圈禁：RAII 句柄（move-only ⇒ 本类不可复制）
    uint64_t size_ = 0;
    InputMeter* meter_ = nullptr;
    uint64_t base_ = 0;
    std::mutex m_;
};

// 区间窗口视图：父视图（文件/spool/另一区间）中 [base, base+len) 的只读视图。
// 嵌套容器免 spool 直读的核心——子引擎把它当作本地小文件随机访问（可链式套窗口）
class RegionView : public SeekView {
public:
    RegionView(std::shared_ptr<RegionSource> parent, uint64_t base, uint64_t len)
        : parent_(std::move(parent)), base_(base), len_(len) {}
    size_t read_at(uint64_t pos, std::span<byte> buf) override {
        if (pos >= len_) return 0;
        uint64_t avail = len_ - pos;
        size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), avail));
        return parent_->read_at(base_ + pos, std::span<byte>(buf.data(), n));
    }
    uint64_t size() const override { return len_; }
private:
    std::shared_ptr<RegionSource> parent_;
    uint64_t base_, len_;
};

// ---- 工厂：挂表纪律类型化（挂表纪律的类型化收口）----
// 纪律（原散于各构造点注释）：根输入文件视图挂 meter（进度/压缩比分母）；
// 码表探测视图不挂（多候选各重读一遍中央目录，会虚增根消耗）；spool/区间
// 派生视图不挂（字节来自外层已计量流，再计即重复）。经命名方法强制选择
struct ViewFactory {
    InputMeter* rootMeter = nullptr;   // 会话根输入计量；探测等临时工厂传空

    // 根输入文件视图（挂表）：zip 根/7z.dll FS 卷（隐写窗口 base/length）
    std::shared_ptr<SeekView> rootFile(const std::wstring& path, uint64_t base = 0,
                                       uint64_t length = 0) const;
    // 码表探测文件视图（不挂表）
    std::shared_ptr<SeekView> probeFile(const std::wstring& path, uint64_t base = 0,
                                        uint64_t length = 0) const;
    // spool 派生（不挂表）：全量或窗口
    static std::shared_ptr<SeekView> spool(std::shared_ptr<SpoolBuffer> s);
    static std::shared_ptr<SeekView> spoolWindow(std::shared_ptr<SpoolBuffer> s,
                                                 uint64_t start, uint64_t len);
    // 父视图区间派生（不挂表）：len=0 到父末尾
    static std::shared_ptr<SeekView> region(std::shared_ptr<RegionSource> parent,
                                            uint64_t base, uint64_t len);
};

} // namespace nx
