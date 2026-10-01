// laimp.hpp：libarchive 侧内部共享件（批次 4 拆分）——错误分类、访问记录、
// 回调上下文与读/seek 回调。仅 laseq/zipcd/open 实现内部使用，勿在公共头引用
#pragma once
#include "bytesource.hpp"
#include "views.hpp"
#include <archive.h>
#include <span>
#include <vector>

namespace nx {

enum class FailKind { Password, Corrupt, Other };

inline FailKind classify_msg(const char* m) {
    if (!m) return FailKind::Other;
    std::string s = ascii_lower(m);
    if (s.find("passphrase") != std::string::npos || s.find("password") != std::string::npos)
        return FailKind::Password;
    if (s.find("crc") != std::string::npos || s.find("damaged") != std::string::npos ||
        s.find("truncat") != std::string::npos || s.find("invalid") != std::string::npos ||
        s.find("unrecognized") != std::string::npos || s.find("unsupported") != std::string::npos ||
        s.find("bad") != std::string::npos || s.find("corrupt") != std::string::npos)
        return FailKind::Corrupt;
    return FailKind::Other;
}

// 数据相位视图访问记录（领域 #3 / 免 spool 直读的区间推导依据）：私有状态机
// idle → active（条目首次数据访问激活）→ 下一条目重置。read/seek 双记——
// libarchive read-ahead 缓冲（256KB）命中时 read 回调不触发，seek 是唯一信号；
// 只消费起点（stored 条目 = 本地头+载荷连续读的锚点，越界无害）
class AccessRecorder {
public:
    void activate() { active_ = true; min_ = UINT64_MAX; }
    void reset() { active_ = false; }
    bool active() const { return active_; }
    void noteRead(uint64_t from, uint64_t to) {   // [from, to) 非空才记
        if (active_ && to > from && from < min_) min_ = from;
    }
    void noteSeek(uint64_t pos) {
        if (active_ && pos < min_) min_ = pos;
    }
    uint64_t firstAccess() const { return min_; }   // UINT64_MAX = 本相位无访问
private:
    bool active_ = false;
    uint64_t min_ = UINT64_MAX;
};

struct CbCtx {
    ByteSource* src = nullptr;      // 流式 = PushbackSource
    SeekView* view = nullptr;       // seekable 模式（spool/文件）
    uint64_t viewPos = 0;           // seekable 模式当前位置
    std::vector<byte> buf;
    AccessRecorder rec;             // 条目数据相位的视图访问记录
};

inline la_ssize_t la_read_cb(archive*, void* c, const void** buf) {
    auto* ctx = static_cast<CbCtx*>(c);
    try {
        size_t n;
        if (ctx->view) {
            uint64_t posBefore = ctx->viewPos;
            n = ctx->view->read_at(ctx->viewPos, ctx->buf);
            ctx->viewPos += n;
            ctx->rec.noteRead(posBefore, ctx->viewPos);   // 数据相位访问记录（区间推导）
        } else {
            n = ctx->src->read(ctx->buf);
        }
        *buf = ctx->buf.data();
        return static_cast<la_ssize_t>(n);
    } catch (...) {
        return -1;
    }
}

inline la_int64_t la_seek_cb(archive*, void* c, la_int64_t off, int whence) {
    auto* ctx = static_cast<CbCtx*>(c);
    if (!ctx->view) return -1;
    try {
        uint64_t size = ctx->view->size();
        uint64_t pos = ctx->viewPos;
        uint64_t abs = 0;
        switch (whence) {
            case SEEK_SET: abs = static_cast<uint64_t>(off); break;
            case SEEK_CUR: abs = pos + static_cast<uint64_t>(off); break;
            case SEEK_END: abs = size + static_cast<uint64_t>(off); break;
            default: return -1;
        }
        // 区间推导记录：数据相位的视图读位置（载荷锚点，本地头由回溯扫描定位）
        ctx->rec.noteSeek(abs);
        ctx->viewPos = abs;
        return static_cast<la_int64_t>(abs);
    } catch (...) {
        return -1;
    }
}

// zip 文件名码表探测（§3.2）：无 EFS 标志 + 无 hdrcharset 时 libarchive 对
// 无法按 UTF-8 校验的名字返回 NULL → 下游消毒成 "_"。对候选码表逐一试开
// （seekable 模式遍历中央目录，不读数据），按"零空名 + 假名加分"择优。

} // namespace nx
