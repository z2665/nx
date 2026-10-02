// shim_main.c：闭包内核的 CSR 边表壳（批次 5）
// 输入（stdin，纯整数）：N M / M 行 "u v"（u→v 强边）/ Q / Q 行查询节点
// 输出：Q 行 0/1（该节点的成员强闭包含自身 = 类型级自引用环）
// 壳是未验证胶水：只做数据搬运；判定语义由 F* 验证的 Closure.closure 保证
#include "krml/internal/compat.h"   // 发行包 krmllib.h 未含 compat——krml_checked_int_t 在此
#include "internal/Closure.h"
#include "internal/Prims.h"
#include "Closure.h"
#include <stdio.h>
#include <stdlib.h>

static uint32_t *adj_off, *adj_tgt, n_nodes, m_edges;
static Prims_list__uint32_t **cache;

static Prims_list__uint32_t *nil_node(void) {
    Prims_list__uint32_t *n = malloc(sizeof *n);
    n->tag = Prims_Nil;
    return n;
}

static Prims_list__uint32_t *edges_of(uint32_t x) {
    if (cache[x]) return cache[x];
    Prims_list__uint32_t *l = nil_node();   // KaRaMeL 惯例：Nil 是实体节点（append 解引用 x->tag）
    for (uint32_t j = adj_off[x + 1]; j-- > adj_off[x];) {
        Prims_list__uint32_t *c = malloc(sizeof *c);
        c->tag = Prims_Cons;
        c->hd = adj_tgt[j];
        c->tl = l;
        l = c;
    }
    cache[x] = l;
    return l;
}

int main(void) {
    if (scanf("%u %u", &n_nodes, &m_edges) != 2) return 2;
    adj_off = calloc(n_nodes + 1, sizeof *adj_off);
    adj_tgt = calloc(m_edges ? m_edges : 1, sizeof *adj_tgt);
    uint32_t *src = calloc(m_edges ? m_edges : 1, sizeof *src);
    for (uint32_t e = 0; e < m_edges; ++e)
        if (scanf("%u %u", &src[e], &adj_tgt[e]) != 2) return 2;
    // 计数排序成 CSR
    for (uint32_t e = 0; e < m_edges; ++e) adj_off[src[e] + 1]++;
    for (uint32_t i = 0; i < n_nodes; ++i) adj_off[i + 1] += adj_off[i];
    uint32_t *pos = malloc((n_nodes + 1) * sizeof *pos);
    for (uint32_t i = 0; i <= n_nodes; ++i) pos[i] = adj_off[i];
    uint32_t *tgt2 = calloc(m_edges ? m_edges : 1, sizeof *tgt2);
    for (uint32_t e = 0; e < m_edges; ++e) tgt2[pos[src[e]]++] = adj_tgt[e];
    for (uint32_t e = 0; e < m_edges; ++e) adj_tgt[e] = tgt2[e];
    free(pos); free(tgt2); free(src);

    cache = calloc(n_nodes, sizeof *cache);
    uint32_t q;
    if (scanf("%u", &q) != 1) return 2;
    for (uint32_t i = 0; i < q; ++i) {
        uint32_t x;
        if (scanf("%u", &x) != 1) return 2;
        // F* 验证的判定：后继种子的 universe_size 轮闭包含 x（≥1 步回到自身）
        printf("%d\n", self_cycle(edges_of, (int32_t)n_nodes, x) ? 1 : 0);
    }
    return 0;
}
