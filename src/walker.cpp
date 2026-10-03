#include "log.hpp"
#include "walker.hpp"
#include "detect.hpp"
#include "filter.hpp"
#include "gui.hpp"
#include "stego.hpp"
#include "volumeset.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>

namespace nx {

namespace {

void layer_note(Session& s, int depth, const std::string& line) {
    (void)s;
    if (log_console_enabled())
        std::printf("%*s%s\n", depth * 2, "", line.c_str());
}

// 名字按最后一个 '.' 拆分（无扩展名返回 false）
bool split_name_ext(const std::string& name, std::string* base, std::string* ext) {
    size_t pos = name.find_last_of('.');
    if (pos == std::string::npos || pos == 0) return false;
    *base = name.substr(0, pos);
    *ext = name.substr(pos + 1);
    return true;
}

// 每层错误语义（设计 §5 D6 + §8）：
//  LimitError → 直接上抛（全局熔断）
//  PasswordExhausted → 分支失败，继续（keep-going 语义对密码默认生效）
//  MissingVolumes → 分支失败，继续
//  CorruptError → keepGoing ? 隔离继续 : 上抛
void handle_branch_error(Session& s, const std::string& where) {
    try {
        throw;
    } catch (LimitError&) {
        s.stats.limitTripped = true;
        throw;
    } catch (PasswordExhausted& e) {
        s.stats.sawPasswordFail = true;
        s.stats.branchesFailed.fetch_add(1);
        log_err("[nx] ✗ %s：%s\n", where.c_str(), e.what());
    } catch (MissingVolumes& e) {
        s.stats.sawMissingVol = true;
        s.stats.branchesFailed.fetch_add(1);
        log_err("[nx] ✗ %s：%s\n", where.c_str(), e.what());
    } catch (CorruptError& e) {
        s.stats.sawCorrupt = true;
        s.stats.corrupt.fetch_add(1);
        log_err("[nx] ✗ %s：%s\n", where.c_str(), e.what());
        if (!s.opt.keepGoing) throw;
    } catch (Error& e) {
        s.stats.branchesFailed.fetch_add(1);
        log_err("[nx] ✗ %s：%s\n", where.c_str(), e.what());
        throw;   // 硬错误（I/O 等）一律中止
    }
}

// 条目级分片组的暂存（M0：按到达序缓存（RAM→溢出），非成员条目到达即封组）
// M1：native=true 为 RAR 原生卷（不拼接，走 7z.dll 卷回调）
struct PendingSet {
    std::string key;
    std::string canonical;          // 子树目录名（native/zspan = key+".rar"/key+".zip"）
    bool zspan = false;             // .zNN + .zip 体系（顺序陷阱：.zip 是终卷）
    bool native = false;            // RAR 原生卷
    std::shared_ptr<SpoolBuffer> spool;
    struct Member {
        unsigned index;        // zspan 终卷 = UINT_MAX
        std::string name;
        uint64_t start, len;
    };
    std::vector<Member> members;
};

void flush_pending_set(Session& s, const LayerCtx& ctx, PendingSet& ps);
void walk(Session& s, std::unique_ptr<ByteSource> src, const LayerCtx& ctx,
          const std::shared_ptr<RegionSource>& region = nullptr);

// ---- 单条目处理 ----
void process_entry(Session& s, ContainerEntry& e, const LayerCtx& ctx) {
    // e.data 的生命周期：本函数内
    auto view = std::make_unique<SharedView>(e.data);
    auto pb = std::make_unique<PushbackSource>(std::move(view), s.opt.histCap);
    Detection d = detect(*pb, e.name);
    FormatClass fc = classify(d.fmt);
    // 免 spool 直读：detect 已 peek → 视图支撑的 stored 条目可给出父区间。
    // 首次为空时对容器类条目再 peek 1MiB 促发底层读（libarchive 缓冲 256KB，
    // 不越过它则回调无记录）；peek 不消费，无副作用。仍为空（非 stored/加密/
    // 小文件整包缓冲）→ open_container 回退 spool 原路径
    std::shared_ptr<RegionSource> region = pb->seekRegion();
    if (!region && fc != FormatClass::None) {
        pb->peek(1 << 20);
        region = pb->seekRegion();
    }
    std::string entryChain = ctx.chain.empty()
                                 ? format_name(d.fmt)
                                 : ctx.chain + " → " + format_name(d.fmt);
    if (fc == FormatClass::Filter || fc == FormatClass::SeqContainer ||
        fc == FormatClass::TailContainer || fc == FormatClass::RandContainer) {
        walk(s, std::move(pb), ctx.forEntry(e.name, std::move(entryChain)), region);
    } else {
        // 普通文件（独立源 → 线程池异步写，D4）
        pb->setHistoryEnabled(false);   // D5：落盘路径无需回看
        std::string rel = ctx.sub.empty() ? e.name : ctx.sub + "/" + e.name;
        s.sink->emitFile(rel, std::shared_ptr<ByteSource>(std::move(pb)), e.size,
                         ctx.depth + 1, e.independentData);
    }
}

// ---- 容器迭代（含条目级分片分组） ----
// reader const&：调用方（walk）在同步调用期间天然持有强引用；异步写出的存活由
// keepAlive 令牌管（批次 4），本参数无需再持所有权
void iterate_container(Session& s, const std::shared_ptr<ContainerReader>& reader,
                       const LayerCtx& ctx) {
    std::vector<PendingSet> pending;
    auto flushAll = [&]() {
        for (auto& ps : pending) {
            try {
                flush_pending_set(s, ctx, ps);
            } catch (...) {
                handle_branch_error(s, "分片组 " + ps.key);
            }
        }
        pending.clear();
    };
    // 把当前条目数据缓存进组
    auto bufferMember = [](Session& s, PendingSet& ps, unsigned index, ContainerEntry& e) {
        uint64_t start = ps.spool->size();
        std::vector<byte> buf(256 << 10);
        for (;;) {
            size_t n = e.data->read(buf);
            if (n == 0) break;
            ps.spool->append(std::span<const byte>(buf.data(), n));
        }
        ps.members.push_back({index, e.name, start, ps.spool->size() - start});
        (void)s;
    };
    ContainerEntry e;
    while (reader->next(e)) {
        if (s.stats.abortFlag.load()) {
            if (gui::progress_cancelled()) throw Cancelled("用户取消");
            throw Error(s.stats.firstHardError.get());
        }
        if (e.isDir) {
            std::string rel = ctx.sub.empty() ? e.name : ctx.sub + "/" + e.name;
            s.sink->emitDir(rel, ctx.depth + 1);
            continue;
        }
        if (e.isSymlink) {
            log_err("[nx] ! 跳过符号链接 %s（v1 降级策略，M0 不落地）\n", e.name.c_str());
            continue;
        }
        auto m = match_split_name(e.name);
        // zspan 终卷：pending 组的 <key>.zip 到达 → 作为最后成员并入（§3.3 顺序陷阱）
        if (!m && !pending.empty() && e.name == pending.back().key + ".zip" &&
            pending.back().zspan) {
            bufferMember(s, pending.back(), UINT_MAX, e);
            continue;
        }
        if (m) {
            // 分片成员：暂存（不同 key 到达 → 先封组；M0 限制：成员须连续到达）
            if (pending.empty() || pending.back().key != m->key) {
                flushAll();
                pending.emplace_back();
                PendingSet& ps = pending.back();
                ps.key = m->key;
                ps.zspan = m->zspan;
                ps.native = m->nativeRar || m->nativeOldRar;
                ps.canonical = ps.native ? m->key + ".rar" : m->key;
                ps.spool = std::make_shared<SpoolBuffer>(s.opt.spoolRam, s.tempDir);
            }
            PendingSet& ps = pending.back();
            // 新式 partN 与旧式 rNN 混在同 key（异常命名）：以先到者为准
            bufferMember(s, ps, m->index, e);
            continue;
        }
        // 普通 .rar：可能是 RAR 旧式首卷（x.rar 后随 x.r00..）→ 暂持为单成员原生组；
        // 后续若来 rNN 则并入，否则按单卷封组（等价于直接走 7z.dll 单卷路径）
        {
            std::string base, ext;
            if (e.name.size() > 4 && split_name_ext(e.name, &base, &ext) &&
                ascii_lower(ext) == "rar") {
                flushAll();   // 与此前组无关联
                pending.emplace_back();
                PendingSet& ps = pending.back();
                ps.key = base;
                ps.native = true;
                ps.canonical = base + ".rar";
                ps.spool = std::make_shared<SpoolBuffer>(s.opt.spoolRam, s.tempDir);
                bufferMember(s, ps, 1, e);
                continue;
            }
        }
        // 普通条目：先冲刷未封组
        flushAll();
        try {
            process_entry(s, e, ctx);
        } catch (...) {
            handle_branch_error(s, "条目 " + e.name);
        }
    }
    flushAll();
}

void flush_pending_set(Session& s, const LayerCtx& ctx, PendingSet& ps) {
    ps.spool->finish();
    // 排序：zspan 时 z01..zNN + 终卷(.zip=UINT_MAX)最后；numbered 按 .001..N
    std::sort(ps.members.begin(), ps.members.end(),
              [](const PendingSet::Member& a, const PendingSet::Member& b) {
                  return a.index < b.index;
              });
    // zspan 缺终卷 = 缺分片
    if (ps.zspan && (ps.members.empty() || ps.members.back().index != UINT_MAX))
        throw MissingVolumes("分片 [" + ps.key + "] 缺少终卷 " + ps.key + ".zip");
    // 完整性预检（§3.3：序号连续无缺口；非末卷等长仅告警）
    VolumeSet vs;
    vs.key = ps.key;
    vs.canonicalName = ps.canonical;
    vs.zipSpan = ps.zspan;
    vs.nativeRar = ps.native;
    for (auto& mem : ps.members) vs.ordered.push_back(VolumeMember{mem.name, mem.len});
    std::string err;
    std::vector<std::string> warns;
    validate_set(vs, &err, &warns);
    for (auto& w : warns) log_err("[nx] ! %s\n", w.c_str());
    if (!err.empty()) throw MissingVolumes(err);

    // ---- RAR 原生卷：7z.dll 卷回调（不拼接，§3.3/D3）----
    if (ps.native) {
        std::map<std::wstring, VolumeSource> vols;
        std::wstring firstVol;
        for (auto& mem : ps.members) {
            // 卷名 = 条目名最后一段（7z.dll 按基名请求兄弟卷）
            std::string base = mem.name;
            size_t slash = base.find_last_of("/\\");
            if (slash != std::string::npos) base = base.substr(slash + 1);
            VolumeSource v;
            v.spool = ps.spool;
            v.winStart = mem.start;
            v.winLen = mem.len;
            vols[utf8_to_wide(base)] = std::move(v);
            if (firstVol.empty()) firstVol = utf8_to_wide(base);
        }
        int newDepth = ctx.depth + 1;
        if (newDepth > s.opt.maxDepth) {
            s.stats.limitTripped = true;
            throw LimitError("递归深度超过上限 " + std::to_string(s.opt.maxDepth));
        }
        LayerId layer = make_layer_id(ctx.logical, ps.canonical, newDepth, "rar");
        layer_note(s, ctx.depth,
                   ps.canonical + " → rar 分卷 x" + std::to_string(ps.members.size()));
        auto reader = open_container_volumes(Format::Rar, vols, firstVol, layer, s.pw,
                                             s.engineOpt());
        s.stats.containers.fetch_add(1);
        LayerCtx child = ctx.forEntry(ps.canonical,
                                      ctx.chain.empty() ? "rar" : ctx.chain);
        child.sub = ctx.sub.empty() ? ps.canonical : ctx.sub + "/" + ps.canonical;
        child.logical = join_logical(ctx.logical, ps.canonical);
        child.depth = newDepth;
        iterate_container(s, std::move(reader), child);
        return;
    }

    // ---- 字节拼接型：ConcatSource 虚拟拼接（D3）----
    std::vector<std::shared_ptr<ByteSource>> parts;
    for (auto& mem : ps.members) {
        parts.push_back(std::make_shared<SpoolBuffer::Window>(ps.spool, mem.start, mem.len));
    }
    layer_note(s, ctx.depth, ps.key + " → 分片 x" + std::to_string(parts.size()));
    walk(s, std::make_unique<ConcatSource>(std::move(parts)),
         ctx.forEntry(ps.key, ctx.chain.empty() ? "分片" : ctx.chain));
}

// ---- walk：策略核心 ----
// region：父视图区间（免 spool 直读；经 detect peek 后由流侧 seekRegion() 提供；
// 过滤器链会剥离——解压后的字节无区间语义）
// ctx const&：只读载体——递归层经 forFilter/descend 工厂派生新 LayerCtx（批次 2）
void walk(Session& s, std::unique_ptr<ByteSource> src, const LayerCtx& ctx,
          const std::shared_ptr<RegionSource>& region) {
    const std::string& origin = ctx.origin;
    auto pb = std::make_unique<PushbackSource>(std::move(src), s.opt.histCap);
    Detection d = detect(*pb, origin);
    FormatClass fc = classify(d.fmt);

    if (fc == FormatClass::Filter) {
        // 决策 D-1：过滤器链纳入 --depth 约束——对抗性嵌套 gzip（4MiB 输入可构造
        // 数千层）原本只在容器分支查深度，过滤器分支同 depth 无限递归 → 栈溢出/
        // 线程耗尽（DoS）。链长按"当前容器段"计，进容器重置
        int newChain = ctx.filterChain + 1;
        if (newChain > s.opt.maxDepth) {
            s.stats.limitTripped = true;
            throw LimitError("过滤器链深度超过上限 " + std::to_string(s.opt.maxDepth) +
                             "（" + origin + "）");
        }
        pb->setHistoryEnabled(false);   // D5：多成员窥探用解码器自有缓冲，不需回看
        s.stats.filters.fetch_add(1);
        std::string ch2 = ctx.chain.empty() ? format_name(d.fmt)
                                           : ctx.chain + " → " + format_name(d.fmt);
        layer_note(s, ctx.depth, origin + " → " + d.display());
        gui::progress_stage("解码 " + origin + "（" + format_name(d.fmt) + "）");
        size_t blocks = std::max<size_t>(2, s.opt.pipeBytes / (256 << 10));
        auto q = std::make_unique<BoundedQueue<std::vector<byte>>>(blocks);
        std::exception_ptr pumpErr = nullptr;
        // D4：每条链每个 FilterStage 一个线程，级间有界缓冲背压
        // D6：压缩比熔断（产出/输入 > maxRatio → LimitError）
        FilterLimiter lim;
        lim.produced = &s.stats.produced;
        lim.inputBytes = &s.meter.bytes;
        if (auto h = pb->sizeHint()) lim.inputFloor = *h;
        lim.maxRatio = s.opt.maxRatio;
        lim.limitTripped = &s.stats.limitTripped;
        // F4：QueueSource 必须先于泵线程构造——若 make_unique 抛出（OOM 窗口），
        // 泵已阻塞在 q.push 且永远等不到消费者，jthread 析构 join 即死锁
        auto qs = std::make_unique<QueueSource>(*q);
        // NOLINT：jthread 回调惯例——stop_token 按值收（本处未用，仅为签名）
        std::jthread pump([&](std::stop_token) {   // NOLINT(performance-unnecessary-value-param)
            filter_decode(d.fmt, *pb, *q, pumpErr, lim);
        });
        // 过滤输出：无区间；qs 随 walk 栈析构（正常/异常皆然）→ abandon → 泵解阻塞
        walk(s, std::move(qs), ctx.forFilter(std::move(ch2), newChain));
        pump.join();   // 先 join 再读 pumpErr（避免竞态）
        if (pumpErr) std::rethrow_exception(pumpErr);
        return;
    }

    if (fc == FormatClass::SeqContainer || fc == FormatClass::TailContainer ||
        fc == FormatClass::RandContainer) {
        int newDepth = ctx.depth + 1;
        if (newDepth > s.opt.maxDepth) {
            s.stats.limitTripped = true;
            throw LimitError("递归深度超过上限 " + std::to_string(s.opt.maxDepth) +
                             "（" + origin + "）");
        }
        LayerId layer = make_layer_id(ctx.logical, origin, newDepth, format_name(d.fmt));
        layer_note(s, ctx.depth, origin + " → " + d.display());
        gui::progress_stage("展开 " + origin + "（" + format_name(d.fmt) + "）");
        auto reader = open_container(std::move(pb), d.fmt, layer, s.pw, s.engineOpt(),
                                     region);
        s.stats.containers.fetch_add(1);
        LayerCtx child = ctx;
        child.origin = origin;
        child.depth = newDepth;
        child.filterChain = 0;
        child.logical = join_logical(ctx.logical, origin);
        // 仅根容器（depth==0）的目录层受 --no-root 抑制；嵌套层照常镜像
        if (ctx.depth == 0 && ctx.sub.empty() && s.opt.noRoot)
            child.sub = "";
        else
            child.sub = ctx.sub.empty() ? origin : ctx.sub + "/" + origin;
        iterate_container(s, std::move(reader), child);
        return;
    }

    // Unknown
    if (ctx.depth == 0 && !ctx.throughFilter) {
        throw Error("无法识别输入格式: " + origin + "（内容嗅探与结构校验均未命中）");
    }
    // 裸过滤器载荷（如 xxx.gz 直接包一个文件）：以剥离过滤后缀的名字落盘
    pb->setHistoryEnabled(false);   // D5
    std::string name = ctx.throughFilter ? strip_filter_suffixes(origin) : origin;
    std::string rel = ctx.sub.empty() ? name : ctx.sub + "/" + name;
    s.sink->emitFile(rel, std::shared_ptr<ByteSource>(std::move(pb)), UINT64_MAX, ctx.depth,
                     false);
}

} // namespace

// M2/M3：根文件免 spool 直读——7z/rar 走 7z.dll；zip 走中央目录 + 码表探测
bool Walker::fsDirectOpen(const std::wstring& path, const std::string& rootName) {
    Session& s = s_;
    auto fsSrc = std::make_shared<FileSource>(path, &s.meter);
    auto pb = std::make_unique<PushbackSource>(std::make_unique<SharedView>(fsSrc), 64 << 10);
    Detection d = detect(*pb, rootName);
    if (d.fmt != Format::SevenZip && d.fmt != Format::Rar && d.fmt != Format::Zip) return false;
    if (d.fmt != Format::Zip && !sevenzip_dll_available())
        return false;   // 惰性：tar 等输入不触发 7z.dll 加载
    if (s.opt.maxDepth < 1) {
        s.stats.limitTripped = true;
        throw LimitError("递归深度上限为 0");
    }
    LayerId layer = make_layer_id("", rootName, 1, format_name(d.fmt));
    layer_note(s, 0, rootName + " → " + d.display() + " [直读]");
    gui::progress_stage("展开 " + rootName + "（" + format_name(d.fmt) + "）");
    std::shared_ptr<ContainerReader> reader;
    if (d.fmt == Format::Zip) {
        // Zip 根：中央目录模式 + 码表探测（§3.2 文件名修复），文件可 seek 免 spool
        reader = open_zip_file(path, layer, s.pw, s.engineOpt());
    } else {
        std::map<std::wstring, VolumeSource> vols;
        VolumeSource v;
        v.fsPath = path;
        vols[utf8_to_wide(rootName)] = std::move(v);
        reader = open_container_volumes(d.fmt, vols, utf8_to_wide(rootName), layer, s.pw, s.engineOpt());
    }
    s.stats.containers.fetch_add(1);
    LayerCtx ctx;
    ctx.sub = s.opt.noRoot ? "" : rootName;
    ctx.origin = rootName;
    ctx.chain = format_name(d.fmt);
    ctx.logical = rootName;
    ctx.depth = 1;
    iterate_container(s, std::move(reader), ctx);
    return true;
}

// ---- 隐写模式（extract-stego / --stego）：只解根文件内藏归档，根文件本体不落盘 ----
// 检测仅限根 FS 层（stego::scan 需 seek 跳过 GB 级 mdat；嵌套流不查）。
void Walker::runStego(const std::wstring& inputPath) {
    Session& s = s_;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(inputPath, ec) || ec)
        throw Error("输入文件不存在: " + wide_to_utf8(inputPath));
    std::string rootName = wide_to_utf8(fs::path(inputPath).filename().wstring());
    auto hit = stego::scan(inputPath);
    if (!hit) {
        s.stats.stegoNotFound = true;
        log_err("[nx] 未检测到隐写压缩包（MP4 原子步进 + 尾部 EOCD 反扫均未命中）: %s\n",
                rootName.c_str());
        return;
    }
    if (s.opt.maxDepth < 1) {
        s.stats.limitTripped = true;
        throw LimitError("递归深度上限为 0");
    }
    std::string fmtName = format_name(hit->fmt);
    LayerId layer{rootName, "第 1 层 " + rootName + " 隐写 (" + fmtName + ")"};
    layer_note(s, 0, rootName + " → 隐写 " + fmtName + " @+" + std::to_string(hit->offset) +
                          " [" + hit->desc + "]");
    gui::progress_stage("展开隐写 " + rootName + "（" + fmtName + "）");
    std::shared_ptr<ContainerReader> reader;
    try {
        if (hit->fmt == Format::Zip) {
            // 尾接 zip：EOCD 精确窗口（基址反推 + 尾部伪装排除）；无区间时整文件直开
            reader = open_zip_file(inputPath, layer, s.pw, s.engineOpt(),
                                   hit->offset, hit->length);
        } else {
            if (!sevenzip_dll_available())
                throw Error("隐写 " + fmtName + " 需要 7z.dll（未找到）");
            VolumeSource v;
            v.fsPath = inputPath;
            v.fsBase = hit->offset;   // [offset, EOF) 窗口 = 干净 7z/rar 流
            std::map<std::wstring, VolumeSource> vols{{L"", std::move(v)}};
            reader = open_container_volumes(hit->fmt, vols, L"", layer, s.pw, s.engineOpt());
        }
    } catch (CorruptError& e) {
        // EOCD/原子头假阳性：按未检测到反馈（密码耗尽等仍照常上抛）
        log_err("[nx] 隐写归档打开失败（疑似假阳性）: %s\n", e.what());
        s.stats.stegoNotFound = true;
        return;
    }
    s.stats.containers.fetch_add(1);
    LayerCtx ctx;
    ctx.sub = s.opt.noRoot ? "" : rootName;
    ctx.origin = rootName;
    ctx.chain = fmtName;
    ctx.logical = rootName;
    ctx.depth = 1;
    iterate_container(s, std::move(reader), ctx);
}

// ---- 根输入 ----
void Walker::run(const std::wstring& inputPath) {
    Session& s = s_;
    if (s.opt.stegoMode) {
        runStego(inputPath);
        return;
    }
    // 文件系统级分片感知（D3）
    std::string volErr;
    auto set = group_filesystem(inputPath, &volErr);
    std::string rootName;
    std::vector<std::shared_ptr<ByteSource>> parts;
    namespace fs = std::filesystem;
    fs::path p(inputPath);
    fs::path dir = p.parent_path();
    if (dir.empty()) dir = L".";

    // 进度分母（待办 #2）：根输入总大小（分片组=各卷之和；多输入在 Stats 上累计）。
    // 取不到大小则累计 0 → 进度窗回退动画条。
    {
        std::error_code ec;
        auto add_size = [&](const fs::path& p) {
            uintmax_t sz = fs::file_size(p, ec);
            if (!ec) s.stats.inputTotal.fetch_add(static_cast<uint64_t>(sz));
        };
        if (set)
            for (auto& m : set->ordered) add_size(dir / fs::path(utf8_to_wide(m.name)));
        else
            add_size(p);
    }

    if (set) {
        for (auto& m : set->ordered)
            parts.push_back(std::make_shared<FileSource>(
                (dir / fs::path(utf8_to_wide(m.name))).wstring(), &s.meter));
        rootName = set->canonicalName;
        if (set->nativeRar) {
            // RAR 原生分卷（FS 级）：7z.dll 卷回调直接读各卷文件，免 spool（§3.3/D3）
            std::map<std::wstring, VolumeSource> vols;
            std::wstring firstVol;
            for (auto& m : set->ordered) {
                VolumeSource v;
                v.fsPath = (dir / fs::path(utf8_to_wide(m.name))).wstring();
                vols[utf8_to_wide(m.name)] = std::move(v);
                if (firstVol.empty()) firstVol = utf8_to_wide(m.name);
            }
            if (s.opt.maxDepth < 1) throw LimitError("递归深度上限为 0");
            layer_note(s, 0, rootName + " → rar 分卷 x" + std::to_string(set->ordered.size()));
            LayerId layer = make_layer_id("", rootName, 1, "rar");
            auto reader = open_container_volumes(Format::Rar, vols, firstVol, layer, s.pw,
                                                 s.engineOpt());
            s.stats.containers.fetch_add(1);
            LayerCtx ctx;
            ctx.sub = rootName;
            ctx.origin = rootName;
            ctx.chain = "rar";
            ctx.logical = rootName;
            ctx.depth = 1;
            iterate_container(s, std::move(reader), ctx);
            return;
        }
        layer_note(s, 0, rootName + " → 分片 x" + std::to_string(parts.size()));
        LayerCtx ctx;
        ctx.origin = rootName;
        ctx.chain = "分片";
        walk(s, std::make_unique<ConcatSource>(std::move(parts)), std::move(ctx));
    } else {
        if (!volErr.empty()) throw MissingVolumes(volErr);
        rootName = wide_to_utf8(p.filename().wstring());
        // M2 快路径：根文件是 7z/rar 且 7z.dll 可用 → 免 spool 直读（IInStream over 文件）
        if (fsDirectOpen(inputPath, rootName))
            return;
        parts.push_back(std::make_shared<FileSource>(inputPath, &s.meter));
        LayerCtx ctx;
        ctx.origin = rootName;
        walk(s, std::make_unique<ConcatSource>(std::move(parts)), std::move(ctx));
    }
}

// 运行期选项装配单点（批次 2 / 领域 #11）：从 main 内联块迁入——spool RAM 自适应
// （空闲物理内存 50%，64MiB–8GiB）+ 溢出临时目录默认=输出目录（tree → 系统临时）
void resolve_runtime_options(Options& opt, std::wstring& tempDir,
                             const std::wstring& outDir, bool dryRun) {
    if (opt.spoolRam == 0) {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        uint64_t avail = ms.ullAvailPhys / 2;
        opt.spoolRam = static_cast<size_t>(
            avail < (64ull << 20) ? (64ull << 20)
                                  : (avail > (8ull << 30) ? (8ull << 30) : avail));
    }
    if (tempDir.empty() && !outDir.empty() && !dryRun)
        tempDir = outDir;
    log_out("[nx] spool RAM %s · 溢出临时目录 %s\n", format_size(opt.spoolRam).c_str(),
            (tempDir.empty() ? "(系统临时目录)" : wide_to_utf8(tempDir)).c_str());
}

// 兼容自由入口（main / fuzz 调用形态不变）
void run_input(Session& s, const std::wstring& inputPath) {
    Walker(s).run(inputPath);
}

} // namespace nx
