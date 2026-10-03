// views.cpp：seekable 三态视图唯一实现（批次 4 合并；原 engines.cpp/szcom.cpp 两份）
#include "views.hpp"

namespace nx {

SpoolWindowView::SpoolWindowView(std::shared_ptr<SpoolBuffer> s, uint64_t start,
                                 uint64_t len)
    : spool_(std::move(s)), start_(start), len_(len) {
    if (len_ == 0) len_ = spool_->size() - start_;
}

size_t SpoolWindowView::read_at(uint64_t pos, std::span<byte> buf) {
    if (pos >= len_) return 0;
    uint64_t avail = len_ - pos;
    size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), avail));
    return spool_->read_at(start_ + pos, std::span<byte>(buf.data(), n));
}

FileSeekView::FileSeekView(const std::wstring& path, InputMeter* meter, uint64_t base,
                           uint64_t length)
    : meter_(meter), base_(base) {
    h_ = res::adopt_file(CreateFileW(win_long_path(path).c_str(), GENERIC_READ,
                                     FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!h_.valid())
        throw Error("打开文件失败: " + wide_to_utf8(path));
    LARGE_INTEGER sz{};
    GetFileSizeEx(h_.get(), &sz);
    uint64_t total = static_cast<uint64_t>(sz.QuadPart);
    if (base > total || base + (length ? length : (total - base)) > total)
        throw Error("视图窗口越界: " + wide_to_utf8(path));
    size_ = length ? length : (total - base);
}

size_t FileSeekView::read_at(uint64_t pos, std::span<byte> buf) {
    std::lock_guard<std::mutex> lk(m_);
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(base_ + pos);
    if (!SetFilePointerEx(h_.get(), li, nullptr, FILE_BEGIN)) throw Error("定位失败");
    size_t got = 0;
    while (got < buf.size()) {
        DWORD r = 0;
        if (!ReadFile(h_.get(), buf.data() + got, static_cast<DWORD>(buf.size() - got), &r,
                      nullptr) || r == 0)
            break;
        got += r;
    }
    if (meter_) meter_->bytes += got;   // 根消耗（重读会被 99% 封顶吸收）
    return got;
}

std::shared_ptr<SeekView> ViewFactory::rootFile(const std::wstring& path, uint64_t base,
                                                uint64_t length) const {
    return std::make_shared<FileSeekView>(path, rootMeter, base, length);
}

std::shared_ptr<SeekView> ViewFactory::probeFile(const std::wstring& path, uint64_t base,
                                                 uint64_t length) const {
    return std::make_shared<FileSeekView>(path, nullptr, base, length);
}

std::shared_ptr<SeekView> ViewFactory::spool(std::shared_ptr<SpoolBuffer> s) {
    return std::make_shared<SpoolWindowView>(std::move(s), 0, 0);
}

std::shared_ptr<SeekView> ViewFactory::spoolWindow(std::shared_ptr<SpoolBuffer> s,
                                                   uint64_t start, uint64_t len) {
    return std::make_shared<SpoolWindowView>(std::move(s), start, len);
}

std::shared_ptr<SeekView> ViewFactory::region(std::shared_ptr<RegionSource> parent,
                                              uint64_t base, uint64_t len) {
    if (len == 0) len = parent->size() - base;
    return std::make_shared<RegionView>(std::move(parent), base, len);
}

} // namespace nx
