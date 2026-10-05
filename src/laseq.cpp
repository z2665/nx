// laseq.cpp：LaSeqReader 实现（自 engines.cpp 拆分）
#include "laseq.hpp"
#include "diag.hpp"
#include "log.hpp"
#include <algorithm>

namespace nx {

class LaEntrySource : public ByteSource {
public:
    // token = 迭代位置（EntryToken 契约：源经 token 显式索取，失效由读取器拒绝）；
    // const&：仅弱引用转换，不需要所有权
    LaEntrySource(const std::shared_ptr<LaSeqReader>& r, EntryToken token)
        : r_(r), token_(token) {}
    size_t read(std::span<byte> buf) override {
        auto r = r_.lock();
        if (!r) throw Error("条目流已失效（读取器已销毁）");
        return r->readEntryData(static_cast<int>(token_.seq), buf);
    }
    // D5：libarchive 块视图直借，省一次 memcpy
    std::span<const byte> read_direct(size_t maxN) override {
        auto r = r_.lock();
        if (!r) throw Error("条目流已失效（读取器已销毁）");
        return r->readEntryDirect(static_cast<int>(token_.seq), maxN);
    }
    std::optional<uint64_t> sizeHint() const override {
        auto r = r_.lock();
        if (!r) return {};
        return r->entrySize(static_cast<int>(token_.seq));
    }
    std::shared_ptr<RegionSource> seekRegion() const override {
        auto r = r_.lock();
        return r ? r->regionOf(static_cast<int>(token_.seq)) : nullptr;
    }
    std::shared_ptr<void> keepAlive() const override {
        auto r = r_.lock();
        return r ? std::shared_ptr<void>(r) : nullptr;   // 别名构造令牌
    }
private:
    std::weak_ptr<LaSeqReader> r_;
    EntryToken token_;
};

bool LaSeqReader::next(ContainerEntry& out) {
    if (!replayQ_.empty()) {
        ReplayRecord rec = std::move(replayQ_.front());
        replayQ_.pop_front();
        out.name = std::move(rec.name);
        out.size = rec.size;
        out.isDir = rec.isDir;
        out.isSymlink = rec.isSymlink;
        out.symlinkTarget = std::move(rec.symlinkTarget);
        out.independentData = false;
        // 重放时重建条目源：目录/空文件本就无数据可读；含数据的停留条目
        // idx == curIdx_（readEntryData 的失效校验依赖它），与 probe 前行为等价
        out.data = rec.nullSrc
                       ? std::shared_ptr<ByteSource>(std::make_shared<NullSource>())
                       : std::make_shared<LaEntrySource>(
                             shared_from_this(), EntryToken{static_cast<uint64_t>(rec.idx)});
        return true;
    }
    return nextInternal(out);
}

bool LaSeqReader::nextInternal(ContainerEntry& out) {
    // drained（设计 §4.1 状态机）：EOF 后不再触碰 libarchive——zip reader 的
    // eof 态结构上二次 next_header 报 FATAL INTERNAL ERROR。全空/纯目录容器
    // 的 probe 轮即会耗尽迭代（无数据条目可停留），主迭代随后必踩此路径
    if (eofHit_) return false;
    int r = archive_read_next_header2(a_, e_.get());
    if (r == ARCHIVE_EOF) {
        eofHit_ = true;
        return false;
    }
    if (r != ARCHIVE_OK && r != ARCHIVE_WARN) {
        FailKind fk = classify_msg(archive_error_string(a_));
        std::string m = archive_error_string(a_) ? archive_error_string(a_) : "读取头失败";
        if (fk == FailKind::Password) throw PasswordExhausted("", "加密层密码错误: " + m);
        throw CorruptError("读取归档头失败: " + m);
    }
    ++curIdx_;
    laBlock_ = {};
    blockOff_ = 0;
    entryPos_ = 0;
    dataPhase_ = false;
    ctx_.rec.reset();
    probeFront_.clear();
    const char* nm = archive_entry_pathname(e_.get());
    if (!nm) {
        // 防御（§3.2）：码表探测后仍可能出现空名（未知编码）——合成可辨识名而非 "_"
        log_err("[nx] ! 条目 %d 名字无法解码（未知码表），已合成占位名\n", curIdx_);
        out.name = "__noname_" + std::to_string(curIdx_);
    } else {
        out.name = fix_name(codec_, nm);   // §3.2 文件名编码：无 EFS 标志的 CP437 乱码修复
    }
    auto ft = archive_entry_filetype(e_.get());   // la_mode_t（MSVC 无 mode_t）
    out.isDir = (ft & AE_IFMT) == AE_IFDIR;
    out.isSymlink = (ft & AE_IFMT) == AE_IFLNK;
    out.symlinkTarget.clear();
    if (out.isSymlink && archive_entry_symlink(e_.get()))
        out.symlinkTarget = archive_entry_symlink(e_.get());
    out.size = archive_entry_size_is_set(e_.get())
                   ? static_cast<uint64_t>(archive_entry_size(e_.get()))
                   : UINT64_MAX;
    sizes_.push_back(out.size);
    out.data = std::make_shared<LaEntrySource>(shared_from_this(),
                                               EntryToken{static_cast<uint64_t>(curIdx_)});
    return true;
}

// 零拷贝：优先当前块残留视图；否则拉新块返回其视图（全程无 memcpy）。
// probeFront_ 非空时退回空视图（重放字节须先经 read() 交付）。
std::span<const byte> LaSeqReader::readEntryDirect(int idx, size_t maxN) {
    if (idx != curIdx_ || maxN == 0) return {};
    if (!probeFront_.empty()) return {};
    activateDataPhase();   // 首次数据访问：激活视图访问记录（区间推导前提）
    if (blockOff_ >= laBlock_.size()) {
        laBlock_ = pullBlock();
        blockOff_ = 0;
        if (laBlock_.empty()) return {};
    }
    size_t n = std::min(maxN, laBlock_.size() - blockOff_);
    auto v = laBlock_.subspan(blockOff_, n);
    blockOff_ += n;
    return v;
}

size_t LaSeqReader::readEntryData(int idx, std::span<byte> buf) {
    if (idx != curIdx_) throw Error("条目流已失效（迭代已前进）");
    if (buf.empty()) return 0;
    if (!dataPhase_ && probeFront_.empty()) activateDataPhase();
    if (!probeFront_.empty()) {   // probe 预读字节优先交付
        size_t n = std::min(buf.size(), probeFront_.size());
        std::memcpy(buf.data(), probeFront_.data(), n);
        probeFront_.erase(probeFront_.begin(), probeFront_.begin() + n);
        return n;
    }
    if (blockOff_ < laBlock_.size()) {
        size_t n = std::min(buf.size(), laBlock_.size() - blockOff_);
        std::memcpy(buf.data(), laBlock_.data() + blockOff_, n);
        blockOff_ += n;
        return n;
    }
    laBlock_ = pullBlock();
    blockOff_ = 0;
    if (laBlock_.empty()) return 0;
    size_t n = std::min(buf.size(), laBlock_.size());
    std::memcpy(buf.data(), laBlock_.data(), n);
    blockOff_ = n;
    return n;
}

// 拉取下一数据块（M3：readEntryData/readEntryDirect 两路径 ~40 行错误处理
// 逐字重复 → 一处）。EOF 返回空 span；CRC 告警/密码/损坏统一在此抛
std::span<const byte> LaSeqReader::pullBlock() {
    const void* p = nullptr;
    size_t sz = 0;
    la_int64_t off = 0;
    int r = archive_read_data_block(a_, &p, &sz, &off);
    if (r == ARCHIVE_EOF) return {};
    const char* emsg = archive_error_string(a_);
    if (r == ARCHIVE_WARN) {   // 告警但数据可用：CRC 失败须上报，其余容忍
        std::string low = ascii_lower(emsg ? emsg : "");
        if (low.find("crc") != std::string::npos)
            throw CorruptError(std::string("条目数据损坏(CRC): ") + (emsg ? emsg : ""));
    } else if (r != ARCHIVE_OK) {
        FailKind fk = classify_msg(emsg);
        if (fk == FailKind::Password)
            throw PasswordExhausted("", std::string("条目密码错误: ") + (emsg ? emsg : ""));
        throw CorruptError(std::string("条目数据损坏: ") + (emsg ? emsg : ""));
    }
    if (off != static_cast<la_int64_t>(entryPos_))
        throw CorruptError("条目数据偏移不连续");
    entryPos_ += sz;
    return std::span<const byte>(static_cast<const byte*>(p), sz);
}

} // namespace nx
