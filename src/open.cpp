// open.cpp：容器打开策略（批次 4 自 engines.cpp 拆分）——try_open 探测、密码迭代、
// spool 兜底、zip 中央目录模式、R 类 7z.dll 优先与回退、S/Z 类流式优先
#include "engines.hpp"
#include "diag.hpp"
#include "laimp.hpp"
#include "laseq.hpp"
#include "zipcd.hpp"
#include "log.hpp"
#include <windows.h>
#include <archive.h>
#include <archive_entry.h>

namespace nx {

namespace {

archive* make_arch(Format fmt) {
    archive* a = archive_read_new();
    if (!a) throw Error("libarchive 分配失败");
    bool ok = true;
    switch (fmt) {
        case Format::Tar: ok = archive_read_support_format_tar(a) == ARCHIVE_OK; break;
        case Format::Cpio: ok = archive_read_support_format_cpio(a) == ARCHIVE_OK; break;
        case Format::Ar: ok = archive_read_support_format_ar(a) == ARCHIVE_OK; break;
        case Format::Zip: ok = archive_read_support_format_zip(a) == ARCHIVE_OK; break;
        case Format::SevenZip: ok = archive_read_support_format_7zip(a) == ARCHIVE_OK; break;
        case Format::Rar:
            ok = archive_read_support_format_rar(a) == ARCHIVE_OK &&
                 archive_read_support_format_rar5(a) == ARCHIVE_OK;
            break;
        case Format::Iso: ok = archive_read_support_format_iso9660(a) == ARCHIVE_OK; break;
        case Format::Cab: ok = archive_read_support_format_cab(a) == ARCHIVE_OK; break;
        default: ok = false;
    }
    if (!ok) {
        archive_read_free(a);
        throw Error("libarchive 不支持该格式: " + std::string(format_name(fmt)));
    }
    return a;
}

struct OpenOutcome {
    std::shared_ptr<LaSeqReader> reader;   // 成功时非空
    FailKind fail = FailKind::Other;
    std::string failMsg;
};

// 打开 + probe（首条目首块验证密码）。不抛异常，失败以 fail/failMsg 表达。
// 流式模式：仅借用 streamingSrc（成功后 caller 调 adoptStream）；失败时所有权不受影响。
OpenOutcome try_open(Format fmt,
                     std::unique_ptr<PushbackSource>& streamingSrc,
                     const std::shared_ptr<SpoolBuffer>& spool,
                     const SecureStr* pw,
                     const std::shared_ptr<SeekView>& view = nullptr,
                     const char* zipCharset = nullptr) {
    OpenOutcome oc;
    diag::TryOpenGuard tog;   // S3：失败出口的读取器必须当场析构（D6 环的案发现场检查）
    archive* a = nullptr;
    try {
        a = make_arch(fmt);
        if (pw && !pw->empty()) archive_read_add_passphrase(a, pw->c_str());
        if (zipCharset && *zipCharset) {
            std::string opt = "zip:hdrcharset=";
            opt += zipCharset;
            archive_read_set_options(a, opt.c_str());
        }
        std::shared_ptr<SeekView> v = view;
        if (!v && spool) v = ViewFactory::spool(spool);
        auto r = std::make_shared<LaSeqReader>(a, archive_entry_new(),
                                               (!spool && !v) ? streamingSrc.get() : nullptr,
                                               spool, v);
        archive_read_set_read_callback(a, la_read_cb);
        archive_read_set_close_callback(a, [](archive*, void*) { return ARCHIVE_OK; });
        if (r->ctx().view) archive_read_set_seek_callback(a, la_seek_cb);
        archive_read_set_callback_data(a, &r->ctx());
        int res = archive_read_open1(a);
        if (res != ARCHIVE_OK) {
            const char* m = archive_error_string(a);
            oc.fail = classify_msg(m);
            oc.failMsg = m ? m : "open 失败";
            return oc;   // r 析构 → 释放 a；streamingSrc 归 caller
        }
        try {
            r->probeFirst();
        } catch (PasswordExhausted& e) {
            oc.fail = FailKind::Password;
            oc.failMsg = e.what();
            return oc;
        } catch (CorruptError& e) {
            oc.fail = FailKind::Corrupt;
            oc.failMsg = e.what();
            return oc;
        } catch (Error& e) {
            oc.fail = FailKind::Other;
            oc.failMsg = e.what();
            return oc;
        }
        if (!spool && !v) r->adoptStream(std::move(streamingSrc));   // 成功：过继
        oc.reader = std::move(r);
        tog.escaped = true;   // 成功路径 reader 存活合法
        return oc;
    } catch (std::exception& e) {
        if (a) archive_read_free(a);   // r 未建立所有权时
        oc.fail = FailKind::Other;
        oc.failMsg = e.what();
        return oc;
    }
}

std::shared_ptr<SpoolBuffer> spool_all(PushbackSource& src, const EngineOptions& opt) {
    auto s = std::make_shared<SpoolBuffer>(opt.spoolRam, opt.tempDir);
    std::vector<byte> buf(256 << 10);
    for (;;) {
        size_t n = src.read(buf);
        if (n == 0) break;
        s->append(std::span<const byte>(buf.data(), n));
    }
    s->finish();
    return s;
}

// 密码候选迭代循环（§6.2 顺序：缓存→上次成功→候选→交互）
// 返回成功 reader；耗尽抛 PasswordExhausted；损坏抛 CorruptError

std::shared_ptr<LaSeqReader> password_loop(Format fmt,
                                           const std::shared_ptr<SpoolBuffer>& spool,
                                           const LayerId& layer,
                                           PasswordProvider& pw,
                                           const char* zipCharset = nullptr) {
    for (;;) {
        auto cand = pw.nextAttempt(layer);
        if (!cand)
            throw PasswordExhausted(layer.display, "密码缺失或已耗尽: " + layer.display);
        std::unique_ptr<PushbackSource> nullSrc{};
        auto oc = try_open(fmt, nullSrc, spool, &*cand, nullptr, zipCharset);
        if (oc.reader) {
            pw.reportSuccess(layer, *cand);
            return std::move(oc.reader);
        }
        if (oc.fail != FailKind::Password)
            throw CorruptError(oc.failMsg);   // 非"密码错"= 数据坏，直接报损坏
    }
}

} // namespace

// 原生多卷入口（RAR）：直接走 7z.dll（§3.3 原生卷型不拼接）
std::shared_ptr<ContainerReader> open_container_volumes(
    Format fmt, const std::map<std::wstring, sz::VolumeSource>& volumes,
    const std::wstring& firstVol, const LayerId& layer, PasswordProvider& pw,
    const EngineOptions& opt) {
    if (!sz::dll_available())
        throw Error(wide_to_utf8(sz::dll_error()) + "（原生多卷需要 7z.dll）");
    return sz::open_archive(fmt, volumes, firstVol, layer, pw, opt);
}

// Zip 根文件直读（中央目录模式 + 码表探测；文件本身可 seek，免 spool）。
// base/length：隐写窗口（EOCD 精确区间，排除尾部伪装数据）；默认整文件。
std::shared_ptr<ContainerReader> open_zip_file(const std::wstring& path,
                                               const LayerId& layer,
                                               PasswordProvider& pw,
                                               const EngineOptions& opt,
                                               uint64_t base, uint64_t length) {
    // 码表探测视图不挂 meter：多候选各重读一遍中央目录，会虚增根消耗计数
    ViewFactory probe;   // 码表探测视图不挂 meter：多候选各重读一遍中央目录，会虚增根消耗
    std::string cs = detect_zip_charset(
        [&] { return probe.probeFile(path, base, length); });
    auto view = ViewFactory{opt.meter}.rootFile(path, base, length);
    std::unique_ptr<PushbackSource> nullSrc{};
    auto oc = try_open(Format::Zip, nullSrc, nullptr, nullptr, view, cs.c_str());
    if (oc.reader) return std::move(oc.reader);
    if (oc.fail == FailKind::Password) {
        // 密码迭代需要重开：在文件视图上直接重试（无需 spool）
        for (;;) {
            auto cand = pw.nextAttempt(layer);
            if (!cand)
                throw PasswordExhausted(layer.display, "密码缺失或已耗尽: " + layer.display);
            auto v2 = ViewFactory{opt.meter}.rootFile(path, base, length);
            auto oc2 = try_open(Format::Zip, nullSrc, nullptr, &*cand, v2, cs.c_str());
            if (oc2.reader) {
                pw.reportSuccess(layer, *cand);
                return std::move(oc2.reader);
            }
            if (oc2.fail != FailKind::Password)
                throw CorruptError(oc2.failMsg);
        }
    }
    throw CorruptError(oc.failMsg);
}

std::shared_ptr<ContainerReader> open_container(std::unique_ptr<PushbackSource> src,
                                                Format fmt,
                                                const LayerId& layer,
                                                PasswordProvider& pw,
                                                const EngineOptions& opt,
                                                const std::shared_ptr<RegionSource>& region) {
    // ---- 免 spool 窗口直读：父视图中的连续 stored 区间即子归档完整字节 ----
    // 仅当调用方携带区间（父为 seekable 视图支撑 + stored 条目）时生效；
    // 视图无流语义，密码重试直接重开同一区间；任何打开失败回退下方 spool 原路径
    //（流未被消费——detect 只 peek，PushbackSource 可从 0 重读）。
    auto regionAsView = std::dynamic_pointer_cast<SeekView>(region);

    // ---- Zip：中央目录模式（seekable）+ 码表探测（§3.2 文件名修复）----
    // 不走本地头流式：无 EFS 标志的本地码表名（CP932/GBK…）在流式下会得到
    // NULL pathname（实测 D:\...\2.zip 案例），中央目录 + hdrcharset 才可靠。
    if (fmt == Format::Zip) {
        if (regionAsView) {
            try {
                std::string cs = detect_zip_charset([&] { return regionAsView; });
                std::unique_ptr<PushbackSource> nullSrc{};
                // 首次无密码直开；密码错则按解析链迭代（视图无流语义，重开即可）
                auto oc = try_open(fmt, nullSrc, nullptr, nullptr, regionAsView,
                                   cs.c_str());
                while (!oc.reader && oc.fail == FailKind::Password) {
                    auto cand = pw.nextAttempt(layer);
                    if (!cand)
                        throw PasswordExhausted(layer.display,
                                                "密码缺失或已耗尽: " + layer.display);
                    oc = try_open(fmt, nullSrc, nullptr, &*cand, regionAsView,
                                  cs.c_str());
                    if (oc.reader) pw.reportSuccess(layer, *cand);
                }
                if (oc.reader) {
                    src.reset();   // 区间打开成功：丢弃未消费的流（子经区间读取）
                    return std::move(oc.reader);
                }
                throw CorruptError(oc.failMsg);   // 区间推导误判等 → 回退 spool
            } catch (PasswordExhausted&) {
                throw;   // 密码耗尽：与 spool 路径数据相同，重试无意义
            } catch (CorruptError&) {
                // 回退 spool 原路径
            } catch (Error&) {
                // 码表探测等异常：回退
            }
        }
        auto spool = spool_all(*src, opt);
        src.reset();
        std::string cs = detect_zip_charset(
            [&] { return ViewFactory::spool(spool); });
        std::unique_ptr<PushbackSource> nullSrc{};
        auto oc = try_open(fmt, nullSrc, spool, nullptr, nullptr, cs.c_str());
        if (oc.reader) return std::move(oc.reader);
        if (oc.fail == FailKind::Password)
            return password_loop(fmt, spool, layer, pw, cs.c_str());
        throw CorruptError(oc.failMsg);
    }

    if (classify(fmt) == FormatClass::RandContainer) {
        // R 类：优先父区间直交 7z.dll（免 spool）；失败回退全量 spool
        if (region && (fmt == Format::SevenZip || fmt == Format::Rar) && sz::dll_available()) {
            try {
                std::map<std::wstring, sz::VolumeSource> vols;
                sz::VolumeSource v;
                v.region = region;
                vols[L""] = std::move(v);
                auto r = sz::open_archive(fmt, vols, L"", layer, pw, opt);
                src.reset();
                return r;
            } catch (PasswordExhausted&) {
                throw;
            } catch (Error&) {
                // 回退 spool
            }
        }
        // R 类：先 spool 全量再随机访问
        auto spool = spool_all(*src, opt);
        src.reset();
        // 7z/rar：优先 7z.dll（全特性 + RAR 解码）；密码耗尽直抛，其余失败回退 libarchive
        if ((fmt == Format::SevenZip || fmt == Format::Rar) && sz::dll_available()) {
            try {
                std::map<std::wstring, sz::VolumeSource> vols;
                sz::VolumeSource v;
                v.spool = spool;
                v.winStart = 0;
                v.winLen = spool->size();
                vols[L""] = std::move(v);
                return sz::open_archive(fmt, vols, L"", layer, pw, opt);
            } catch (PasswordExhausted&) {
                throw;
            } catch (Error&) {
                // 7z.dll 失败 → libarchive 兜底
            }
        }
        std::unique_ptr<PushbackSource> nullSrc{};
        auto oc = try_open(fmt, nullSrc, spool, nullptr);
        if (oc.reader) return std::move(oc.reader);
        if (oc.fail == FailKind::Password) {
            auto r = password_loop(fmt, spool, layer, pw);
            return std::move(r);
        }
        throw CorruptError(oc.failMsg);
    }

    // S/Z 类：流式优先
    auto oc = try_open(fmt, src, nullptr, nullptr);
    if (oc.reader) return std::move(oc.reader);

    if (oc.fail == FailKind::Password) {
        // 加密层需要重启验证 → rewind + spool（D2 语义）
        src->rewindTo(0);
        auto spool = spool_all(*src, opt);
        src.reset();
        return password_loop(fmt, spool, layer, pw);
    }
    if (oc.fail == FailKind::Corrupt) {
        // SFX / 追加修改 / 本地头流式盲区 → D2 回退 spool + 中央目录模式
        try {
            src->rewindTo(0);
        } catch (Error&)        {
            throw CorruptError("流式读取失败且回退窗口不足: " + oc.failMsg);
        }
        auto spool = spool_all(*src, opt);
        src.reset();
        std::unique_ptr<PushbackSource> nullSrc{};
        auto oc2 = try_open(fmt, nullSrc, spool, nullptr);
        if (oc2.reader) return std::move(oc2.reader);
        if (oc2.fail == FailKind::Password) {
            return password_loop(fmt, spool, layer, pw);
        }
        throw CorruptError(oc2.failMsg);
    }
    throw CorruptError(oc.failMsg.empty() ? "容器打开失败" : oc.failMsg);
}

} // namespace nx
