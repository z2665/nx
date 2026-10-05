// laseq.hpp：LaSeqReader——libarchive 顺序条目读取器
#pragma once
#include "container.hpp"
#include "pushback.hpp"
#include "laimp.hpp"
#include "namecodec.hpp"
#include "spool.hpp"
#include "views.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <deque>
#include <memory>

namespace nx {

class LaEntrySource;

// ---- LaSeqReader：libarchive 顺序条目读取器 ----

class LaSeqReader : public ContainerReader, public std::enable_shared_from_this<LaSeqReader> {
public:
    // 三种打开形态：borrowed（流式借用探测）/ view（seekable：spool 或根文件）
    // 流式模式成功后由 caller 调 adoptStream() 过继所有权
    LaSeqReader(archive* a, archive_entry* e,
                PushbackSource* borrowed,
                std::shared_ptr<SpoolBuffer> spool,
                std::shared_ptr<SeekView> view = nullptr)
        : a_(a), e_(e, &archive_entry_free), borrowed_(borrowed),
          spool_(std::move(spool)), view_(std::move(view)) {
        if (view_) {
            ctx_.view = view_.get();
            ctx_.viewPos = 0;
            ctx_.src = nullptr;
        } else {
            ctx_.src = borrowed_;
        }
        (void)spool_;
        ctx_.buf.assign(256 << 10, 0);
    }

    ~LaSeqReader() override {
        if (a_) archive_read_free(a_);
    }

    // 流式模式成功确认后过继所有权（失败路径 caller 保留 src 以便 rewind/spool 回退）
    // 采纳后该流不再需要回看（probe/D2 回退已完成）→ 关历史，启用 D5 直通
    void adoptStream(std::unique_ptr<PushbackSource> s) {
        borrowed_ = nullptr;
        streamingSrc_ = std::move(s);
        ctx_.src = streamingSrc_.get();
        streamingSrc_->setHistoryEnabled(false);
    }

    CbCtx& ctx() { return ctx_; }
    archive* arch() { return a_; }

    // 免 spool 直读：当前条目若为父视图（文件/spool）中的连续 stored 载荷，
    // 返回其精确区间视图。前提：条目数据已经开始经本读取器读取（detect peek /
    // probe 预读皆可）——数据相位首个视图读覆盖本地头，由此解析载荷起点。
    // 解析失败（非 stored/加密/头不合法/未进入数据相位）返回 null → 调用方回退 spool。
    std::shared_ptr<RegionSource> regionOf(int idx) {
        if (!view_ || idx != curIdx_ || !dataPhase_ || sizes_.empty()) return nullptr;
        uint64_t esz = sizes_[std::min<size_t>(idx, sizes_.size() - 1)];
        if (esz == UINT64_MAX || !ctx_.rec.active() || ctx_.rec.firstAccess() == UINT64_MAX) {
            return nullptr;
        }
        // firstAccess = 数据相位首个视图读位置（载荷中段或本地头）。
        // 向前回溯定位本地头：PK\x03\x04 + method==0 + 未加密 + 覆盖 firstAccess
        // 且长度精确 = esz。zip 条目区间互不重叠 → 覆盖 firstAccess 的 stored 载荷至多
        // 一个；误配由调用方的"子打开失败回退 spool"兜底。
        // 覆盖 libarchive 首块缓冲（256KB）+ 本地头/扩展字段上限（30+64K+64K）
        const uint64_t back = 512 * 1024;
        uint64_t first = ctx_.rec.firstAccess();
        uint64_t from = first > back ? first - back : 0;
        size_t spanLen = static_cast<size_t>(first - from) + 30;
        std::vector<byte> scan(spanLen);
        if (view_->read_at(from, std::span<byte>(scan)) != spanLen) return nullptr;
        for (size_t p = 0; p + 30 <= scan.size(); ++p) {
            if (scan[p] != 'P' || scan[p + 1] != 'K' || scan[p + 2] != 0x03 ||
                scan[p + 3] != 0x04)
                continue;
            unsigned flags = scan[p + 6] | (scan[p + 7] << 8);
            if (flags & 0x1) continue;   // 加密（12B 密码头在载荷前）→ 回退
            unsigned method = scan[p + 8] | (scan[p + 9] << 8);
            if (method != 0) continue;   // 仅 stored
            unsigned nlen = scan[p + 26] | (scan[p + 27] << 8);
            unsigned elen = scan[p + 28] | (scan[p + 29] << 8);
            // 头自证（P0 修复）：stored 条目的本地头 csize==usize==条目尺寸——
            // 压缩流/相邻结构里偶合出现的 PK\x03\x04+method0 垃圾魔数在此排除。
            // 修复前 deflate 外层条目可被误推区间（垃圾头位置当载荷起点），错误
            // 窗口打开内层后中途读头失败/状态机违反——触发随流字节巧合漂移。
            // zip64 影子值（0xFFFFFFFF 对）与数据描述符形态（字段为 0）同样
            // 不匹配而回退 spool——保守正确，>4GiB stored 直读留待需要时加
            // extra 字段解析
            unsigned csize = scan[p + 18] | (scan[p + 19] << 8) |
                             (scan[p + 20] << 16) | (static_cast<unsigned>(scan[p + 21]) << 24);
            unsigned usize = scan[p + 22] | (scan[p + 23] << 8) |
                             (scan[p + 24] << 16) | (static_cast<unsigned>(scan[p + 25]) << 24);
            if (csize != usize || csize != static_cast<unsigned>(esz)) continue;
            uint64_t payload = from + p + 30 + nlen + elen;
            if (payload > first) continue;                     // 载荷须始于首读前
            if (first >= payload + esz) continue;              // 首读须落在载荷内
            if (payload + esz > view_->size()) continue;
            return ViewFactory::region(view_, payload, esz);
        }
        return nullptr;
    }

    bool next(ContainerEntry& out) override;

private:
    bool nextInternal(ContainerEntry& out);
    // 拉取下一数据块（M3：readEntryData/readEntryDirect 两路径 ~40 行错误处理
    // 逐字重复 → 一处）。EOF 返回空 span；CRC 告警/密码/损坏统一在此抛
    std::span<const byte> pullBlock();
    // 首次数据访问激活视图访问记录（区间推导前提）
    void activateDataPhase() {
        if (dataPhase_) return;
        dataPhase_ = true;
        if (view_) ctx_.rec.activate();
    }
public:

    size_t readEntryData(int idx, std::span<byte> buf);
    std::span<const byte> readEntryDirect(int idx, size_t maxN);
    std::optional<uint64_t> entrySize(int idx) const {
        if (idx < 0 || idx >= static_cast<int>(sizes_.size())) return {};
        if (sizes_[idx] == UINT64_MAX) return {};
        return sizes_[idx];
    }

    // 探测：前进到首个含数据的条目并读首块（验证密码）。失败经异常（密码/损坏）。
    // 途经的目录/空文件条目元数据进 replayQ_ 由 next() 重放（顺序保持）；
    // 停在的条目预读字节存 probeFront_ 重放给下游（否则条目流短 1 字节）。
    // 7z 等工具会把目录条目放在最前——只看首条目会漏掉密码验证。
    void probeFirst() {
        for (;;) {
            ContainerEntry tmp;
            if (!nextInternal(tmp)) return;   // 空容器：open 本身即验证
            ReplayRecord rec{curIdx_, std::move(tmp.name), tmp.size, tmp.isDir, tmp.isSymlink,
                             false, std::move(tmp.symlinkTarget)};
            if (tmp.isSymlink || tmp.isDir) {
                replayQ_.push_back(std::move(rec));
                continue;
            }
            if (tmp.size == 0) {   // 空文件：无数据可验，重放时换空流
                rec.nullSrc = true;
                replayQ_.push_back(std::move(rec));
                continue;
            }
            byte b[1];
            size_t got = readEntryData(curIdx_, std::span<byte>(b, 1));
            probeFront_.assign(b, b + got);
            replayQ_.push_back(std::move(rec));
            return;
        }
    }

private:
    friend class LaEntrySource;
    archive* a_;
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)> e_;
    PushbackSource* borrowed_ = nullptr;      // 探测阶段借用
    std::unique_ptr<PushbackSource> streamingSrc_;   // 过继后所有
    std::shared_ptr<SpoolBuffer> spool_;
    std::shared_ptr<SeekView> view_;          // seekable 模式（spool/文件）
    CbCtx ctx_;
    NameCodec codec_;                   // §3.2 码表探测（每读取器粘性）
    // probe 预取条目重放队列（D6 结构修复）：只存元数据 + 迭代序号，绝不持条目源——
    // LaEntrySource 经 shared_from_this() 持回指读取器的强引用，存进读取器自己的
    // 队列即构成自引用环（15GB 临时文件残留案例根因）：打开失败或中途弃置的读取器
    // 永不析构，连带 spool 与视图泄漏。重放在 next() 现场按 idx 重建源
    struct ReplayRecord {
        int idx;
        std::string name;
        uint64_t size;
        bool isDir, isSymlink, nullSrc;   // nullSrc：size==0 空文件，重放换 NullSource
        std::string symlinkTarget;
    };
    std::deque<ReplayRecord> replayQ_;
    std::vector<byte> probeFront_;         // probe 预读待重放字节
    int curIdx_ = -1;
    std::span<const byte> laBlock_;   // 当前 libarchive 块视图（有效至下一次 data_block 调用）
    size_t blockOff_ = 0;
    uint64_t entryPos_ = 0;      // 已拉入 leftover 的条目内偏移
    std::vector<uint64_t> sizes_;
    bool dataPhase_ = false;     // 当前条目已开始数据读取（激活视图访问记录）
    bool eofHit_ = false;        // drained：libarchive 已返 EOF，后续 next 恒 EOF
};

// 条目源：对读取器只持弱引用（所有权模型 fixed 变体——父方向强边消除，
// 强所有权图从此无环；异步存活由 Sink 任务经 keepAlive() 令牌配套保活）

} // namespace nx
