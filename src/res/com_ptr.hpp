// res/com_ptr.hpp —— COM 引用收编（P2 圈禁）。
// 纪律：Release 调用自此只允许出现在 src/res/ 与 COM 接口实现体内
// （IUnknown::Release override 是接口契约，不属手工释放）。
#pragma once
#include "gsl_owner.hpp"
#include <utility>

namespace nx::res {

template <typename T>
class com_ptr {
public:
    com_ptr() = default;
    // 接管既有引用（计数已归我）——参数为 owner：所有权从此处转入，
    // 裸 new 的结果可直入，get() 的借用值传入会被 owning-memory 检查拦下
    explicit com_ptr(gsl::owner<T*> p) noexcept : p_(p) {}
    ~com_ptr() {
        if (p_) p_->Release();
    }
    com_ptr(com_ptr&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    com_ptr& operator=(com_ptr&& o) noexcept {
        if (this != &o) {
            if (p_) p_->Release();
            p_ = std::exchange(o.p_, nullptr);
        }
        return *this;
    }
    com_ptr(const com_ptr&) = delete;
    com_ptr& operator=(const com_ptr&) = delete;

    T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    // 所有权逃逸点（gsl::owner 标注）：转移给"将长期持有"的成员/进程级缓存
    gsl::owner<T*> release() noexcept { return std::exchange(p_, nullptr); }
    void reset(T* p = nullptr) noexcept {
        T* old = std::exchange(p_, p);
        if (old) old->Release();
    }
    // 接收端参数（先释放旧值）：QueryInterface/CreateObject/GetItemAt 的 out 端
    T** out() noexcept {
        reset();
        return &p_;
    }

private:
    T* p_ = nullptr;
};

} // namespace nx::res
