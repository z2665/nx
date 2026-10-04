// layer.hpp：walk 递归的层上下文
// descend 规则唯一化：进入嵌套容器 = sub/logical 延伸 + 深度+1 + 过滤器链清零；
// 进入过滤器 = 链+1（深度/逻辑路径不变——过滤器非密码层）；条目帧 = 换 origin/链。
// sub 受 --no-root 影响（输出布局），logical 不受（密码层身份，兄弟分支必须区分）。
#pragma once
#include "password.hpp"
#include <string>
#include <string_view>

namespace nx {

struct LayerCtx {
    std::string sub;             // 输出相对路径前缀（'/' 连接；根层受 --no-root 抑制）
    std::string origin;          // 当前流名字（条目名自带容器内目录，如 "dir1/a.zip"）
    std::string chain;           // 展示链（含当前层格式，"gzip → tar"）
    std::string logical;         // 父容器逻辑路径（密码缓存键基；过滤器不延伸）
    int depth = 0;               // 容器深度（层编号 + maxDepth 熔断）
    int filterChain = 0;         // 当前容器段内过滤器链长（决策 D-1）
    bool throughFilter = false;  // 流经过滤器（裸载荷命名去后缀）

    // 容器内条目帧：换名字与展示链，回到"新容器段"（链清零、非过滤器）
    LayerCtx forEntry(std::string name, std::string chainExt) const {
        LayerCtx c = *this;
        c.origin = std::move(name);
        c.chain = std::move(chainExt);
        c.filterChain = 0;
        c.throughFilter = false;
        return c;
    }
    // 过滤器帧：链延伸 +1，深度/逻辑路径不变
    LayerCtx forFilter(std::string chainExt, int newChain) const {
        LayerCtx c = *this;
        c.chain = std::move(chainExt);
        c.filterChain = newChain;
        c.throughFilter = true;
        return c;
    }
};

// 容器逻辑路径连接（密码缓存键；空父 = 根）
inline std::string join_logical(const std::string& parent, const std::string& name) {
    return parent.empty() ? name : parent + "/" + name;
}

// 容器层身份（key = 逻辑路径延伸；display = 层编号展示文本）
inline LayerId make_layer_id(const std::string& parentLogical, const std::string& origin,
                             int depth, std::string_view fmtName) {
    return LayerId{join_logical(parentLogical, origin),
                   "第 " + std::to_string(depth) + " 层 " + origin + " (" +
                       std::string(fmtName) + ")"};
}

} // namespace nx
