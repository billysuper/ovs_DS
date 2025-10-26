# Flow Wildcards (wc) 完整变化追踪

## 概述

本文档详细追踪 `struct flow_wildcards *wc` 在 OVS 数据包处理流程中的完整生命周期和变化过程。

---

## 目录

1. [wc 的定义](#wc-的定义)
2. [完整调用链路](#完整调用链路)
3. [阶段 1: 初始化](#阶段-1-初始化)
4. [阶段 2: Upcall 处理](#阶段-2-upcall-处理)
5. [阶段 3: Xlate 转换](#阶段-3-xlate-转换)
6. [阶段 4: Classifier 查找](#阶段-4-classifier-查找)
7. [阶段 5: Subtable 遍历](#阶段-5-subtable-遍历)
8. [阶段 6: Staged Lookup](#阶段-6-staged-lookup)
9. [阶段 7: Wildcards 累积](#阶段-7-wildcards-累积)
10. [阶段 8: Megaflow 安装](#阶段-8-megaflow-安装)
11. [完整示例追踪](#完整示例追踪)
12. [内存布局变化](#内存布局变化)

---

## wc 的定义

### 数据结构

```c
// lib/flow.h
struct flow_wildcards {
    struct flow masks;  // 每个字段的 mask
};

struct flow {
    /* L2 */
    struct eth_addr dl_dst;
    struct eth_addr dl_src;
    ovs_be16 dl_type;
    uint8_t pad1[2];
    ovs_be32 vlans[FLOW_MAX_VLAN_HEADERS];
    ovs_be32 mpls_lse[ROUND_UP(FLOW_MAX_MPLS_LABELS, 2)];
    
    /* L3 */
    ovs_be32 nw_src;
    ovs_be32 nw_dst;
    struct in6_addr ipv6_src;
    struct in6_addr ipv6_dst;
    ovs_be32 ipv6_label;
    uint8_t nw_frag;
    uint8_t nw_tos;
    uint8_t nw_ttl;
    uint8_t nw_proto;
    
    /* L4 */
    ovs_be16 tp_src;
    ovs_be16 tp_dst;
    ovs_be16 tcp_flags;
    ovs_be16 pad2;
    
    /* Metadata */
    ovs_be64 metadata;
    uint32_t regs[FLOW_N_REGS];
    uint32_t pkt_mark;
    uint32_t dp_hash;
    // ... 还有更多字段
};
```

### Wildcards 语义

```
wc->masks.field = 0x00000000  => 完全 wildcard (不关心该字段)
wc->masks.field = 0xFFFFFFFF  => 完全 exact match (必须精确匹配)
wc->masks.field = 0xFFFFFF00  => 部分 wildcard (如 /24 网络掩码)
```

---

## 完整调用链路

```
┌─────────────────────────────────────────────────────────────────┐
│ 1. Kernel Datapath                                              │
│    - 数据包到达，未命中 megaflow                                │
│    - 触发 upcall 到 userspace                                   │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ 2. ofproto/ofproto-dpif-upcall.c: recv_upcalls()                │
│    - 从 kernel 接收 upcall                                      │
│    - 解析数据包和 flow                                          │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ 3. handle_upcalls()                                             │
│    - 批处理多个 upcall                                          │
│    - 为每个 upcall 分配 upcall 结构                             │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ 4. process_upcall()                                             │
│    - 根据 upcall 类型处理                                       │
│    - MISS_UPCALL: 需要完整的 flow translation                  │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ 5. upcall_xlate()  ⭐ WC 从这里开始                            │
│    struct flow_wildcards wc;   // 栈上分配                     │
│    memset(&wc.masks, 0, sizeof wc.masks);  // 初始化为全 0     │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ 6. xlate_actions(&xin, &xout)                                   │
│    xin.wc = &wc;  // 传入 wc 指针                              │
│    - Flow translation: 根据 OpenFlow 规则转换                  │
│    - 调用 classifier 查找匹配规则                              │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ 7. lib/classifier.c: classifier_lookup()                        │
│    classifier_lookup__(cls, version, flow, wc, ...)             │
│    - 遍历 subtables                                             │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ 8. PVECTOR_FOR_EACH_PRIORITY (subtable, ...)                   │
│    for each subtable in priority order:                         │
│      match = find_match_wc(subtable, ..., wc)  ⭐ WC 不断累积  │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ 9. find_match_wc()  ⭐ 单个 subtable 处理                      │
│    - Staged lookup: 多阶段 hash 查找                           │
│    - 成功: flow_wildcards_fold_minimask(wc, mask)              │
│    - 失败: flow_wildcards_fold_minimask_in_map(wc, mask, map)  │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ 10. flow_wildcards_fold_minimask()                              │
│     flow_union_with_miniflow(&wc->masks, &mask->masks)          │
│     - 按位 OR 操作: wc->masks.field |= mask_value              │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ 11. 返回到 xlate_actions()                                      │
│     - wc 现在包含所有检查过的字段                              │
│     - xout 包含要执行的动作                                    │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ 12. ukey_create_from_upcall(upcall, &wc)                        │
│     - 使用 wc 创建 megaflow key                                │
│     - odp_flow_key_from_mask(&wc->masks, &maskbuf)             │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ↓
┌─────────────────────────────────────────────────────────────────┐
│ 13. dpif_operate(DPIF_OP_FLOW_PUT)                              │
│     - 将 megaflow 安装到 kernel datapath                       │
│     - Match: flow + wc->masks                                   │
│     - Actions: xout 中的动作                                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 阶段 1: 初始化

### 代码位置
`ofproto/ofproto-dpif-upcall.c: upcall_xlate()`

### 代码

```c
static void
upcall_xlate(struct udpif *udpif, struct upcall *upcall,
             struct ofpbuf *odp_actions, struct flow_wildcards *wc)
{
    struct dpif_flow_stats stats;
    enum xlate_error xerr;
    struct xlate_in xin;
    
    // ... 初始化 stats ...
    
    // 关键：初始化 xin，传入 wc 指针
    xlate_in_init(&xin, upcall->ofproto,
                  ofproto_dpif_get_tables_version(upcall->ofproto),
                  upcall->flow, upcall->ofp_in_port, NULL,
                  stats.tcp_flags, upcall->packet, 
                  wc,              // ⭐ 传入 wc 指针
                  odp_actions);
    
    // ... 其他初始化 ...
    
    // 调用 xlate_actions，wc 在这个过程中被修改
    xerr = xlate_actions(&xin, &upcall->xout);
    
    // ... 错误处理 ...
}
```

### wc 状态

```c
// 调用 upcall_xlate() 之前
// wc 是 handle_upcalls() 或 upcall_cb() 中定义的局部变量

// 在 upcall_xlate() 被调用时
wc->masks = {
    .dl_dst    = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // wildcard
    .dl_src    = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // wildcard
    .dl_type   = 0x0000,                                 // wildcard
    .nw_src    = 0x00000000,                            // wildcard
    .nw_dst    = 0x00000000,                            // wildcard
    .tp_src    = 0x0000,                                // wildcard
    .tp_dst    = 0x0000,                                // wildcard
    // ... 所有其他字段都是 0 (wildcard)
}
```

**关键点**: 
- wc 在栈上分配（或作为参数传入）
- 初始状态：所有字段都是 wildcard (0)
- 意味着：任何数据包都会匹配（如果只看 wc）

---

## 阶段 2: Upcall 处理

### 代码位置
`ofproto/ofproto-dpif-upcall.c: process_upcall()`

### 代码

```c
static int
process_upcall(struct udpif *udpif, struct upcall *upcall,
               struct ofpbuf *odp_actions, struct flow_wildcards *wc)
{
    const struct dp_packet *packet = upcall->packet;
    const struct flow *flow = upcall->flow;
    
    switch (upcall->type) {
    case MISS_UPCALL:
    case SLOW_PATH_UPCALL:
        // 调用 xlate，wc 会被填充
        upcall_xlate(udpif, upcall, odp_actions, wc);
        return 0;
        
    case SFLOW_UPCALL:
        // sFlow 采样，不需要 megaflow
        // wc 不会被修改
        break;
        
    // ... 其他类型 ...
    }
}
```

### wc 状态

**MISS_UPCALL 路径** (最常见):
- wc 会传递给 `upcall_xlate()`
- 在 xlate 过程中被填充

**其他类型的 upcall**:
- wc 可能不会被修改
- 或者只做简单的初始化

---

## 阶段 3: Xlate 转换

### 代码位置
`ofproto/ofproto-dpif-xlate.c: xlate_actions()`

### 简化代码

```c
enum xlate_error
xlate_actions(struct xlate_in *xin, struct xlate_out *xout)
{
    struct xlate_ctx ctx;
    
    // 初始化 xlate context
    xlate_ctx_init(&ctx, xin->ofproto, xin);
    
    // ctx.wc 指向 xin->wc
    // 即我们传入的 wc
    
    // 执行各种转换
    do_xlate_actions(xin->ofpacts, xin->ofpacts_len, &ctx, ...);
    
    // 在转换过程中会调用 classifier_lookup()
    // classifier_lookup() 会修改 wc
    
    return ctx.error;
}
```

### 关键点

- xlate 过程中会多次调用 classifier 查找流表
- 每次查找都会累积 wildcards 信息
- wc 指针在整个过程中保持不变

---

## 阶段 4: Classifier 查找

### 代码位置
`lib/classifier.c: classifier_lookup()`

### 代码

```c
const struct cls_rule *
classifier_lookup(const struct classifier *cls, ovs_version_t version,
                  struct flow *flow, struct flow_wildcards *wc,
                  struct hmapx *conj_flows)
{
    // 调用内部实现
    return classifier_lookup__(cls, version, flow, wc, true, conj_flows);
}

static const struct cls_rule *
classifier_lookup__(const struct classifier *cls, ovs_version_t version,
                    struct flow *flow, struct flow_wildcards *wc,
                    bool allow_conjunctive_matches,
                    struct hmapx *conj_flows)
{
    struct trie_ctx trie_ctx[CLS_MAX_TRIES];
    const struct cls_match *match;
    const struct cls_match *hard = NULL;
    int hard_pri = INT_MIN;
    
    // 初始化 trie context
    for (uint32_t i = 0; i < n_tries; i++) {
        trie_ctx_init(&trie_ctx[i], &cls->tries[i]);
    }
    
    // ⭐ 主循环：遍历所有 subtable
    struct cls_subtable *subtable;
    PVECTOR_FOR_EACH_PRIORITY (subtable, hard_pri + 1, 2, sizeof *subtable,
                               &cls->subtables) {
        
        // ⭐ 关键调用：每个 subtable 都会修改 wc
        match = find_match_wc(subtable, version, flow, trie_ctx, n_tries, wc);
        
        if (!match || match->priority <= hard_pri) {
            continue;  // wc 已经被修改（累积了检查的字段）
        }
        
        // 找到更高优先级的匹配
        hard = match;
        hard_pri = hard->priority;
    }
    
    // 返回最高优先级的匹配
    return hard ? hard->cls_rule : NULL;
}
```

### wc 状态变化

```
进入 classifier_lookup__():
  wc->masks = 全 0 (wildcard)

第一次调用 find_match_wc(subtable_A, ..., wc):
  wc->masks.nw_src = 0x00000000
  wc->masks.nw_src |= 0xFFFFFF00  (Subtable A 的 mask)
  wc->masks.nw_src = 0xFFFFFF00   ⭐ 第一次累积

第二次调用 find_match_wc(subtable_B, ..., wc):
  wc->masks.nw_dst = 0x00000000
  wc->masks.nw_dst |= 0xFFFFFF00  (Subtable B 的 mask)
  wc->masks.nw_dst = 0xFFFFFF00   ⭐ 第二次累积
  
  wc->masks.nw_src 保持 0xFFFFFF00  (之前的值保留)

第三次调用 find_match_wc(subtable_C, ..., wc):
  wc->masks.dl_type = 0x0000
  wc->masks.dl_type |= 0xFFFF     (Subtable C 的 mask)
  wc->masks.dl_type = 0xFFFF      ⭐ 第三次累积
  
  wc->masks.nw_src 保持 0xFFFFFF00  (之前的值继续保留)
  wc->masks.nw_dst 保持 0xFFFFFF00  (之前的值继续保留)

离开 classifier_lookup__():
  wc->masks = {
    .dl_type = 0xFFFF,      // 来自 Subtable C
    .nw_src  = 0xFFFFFF00,  // 来自 Subtable A
    .nw_dst  = 0xFFFFFF00,  // 来自 Subtable B
    // 其他字段仍然是 0
  }
```

**关键观察**:
- wc 是同一个指针，在整个循环中不变
- 每次调用 `find_match_wc()` 都会通过 `|=` 累积字段
- 之前累积的值永远不会被清除或覆盖

---

## 阶段 5: Subtable 遍历

### 代码位置
`lib/classifier.c: classifier_lookup__()`  主循环

### 详细追踪

```c
// 假设有 3 个 subtable，优先级递减
Subtable A: priority 100, mask {nw_src=255.255.255.0, tp_dst=0xFFFF}
Subtable B: priority 50,  mask {nw_dst=255.255.255.0, tp_src=0xFFFF}
Subtable C: priority 10,  mask {dl_type=0xFFFF}

// 输入 flow
flow = {
    dl_type = 0x0800,
    nw_src  = 172.16.0.100,
    nw_dst  = 192.168.1.50,
    tp_src  = 12345,
    tp_dst  = 8080,
}

// wc 初始状态
wc->masks = 全 0
```

### 迭代过程

#### 迭代 1: Subtable A

```c
PVECTOR_FOR_EACH_PRIORITY 第 1 次迭代:
  subtable = Subtable A (priority 100)
  hard_pri + 1 = INT_MIN + 1 (允许检查)
  
  调用: match = find_match_wc(Subtable A, ..., wc)
  
  在 find_match_wc() 内部:
    // Staged lookup 检查
    检查 nw_src: 172.16.0.0 vs 10.0.0.0 => 不匹配
    
    // 累积 wildcards (即使不匹配也要累积)
    wc->masks.nw_src |= 0xFFFFFF00  => 0xFFFFFF00
    wc->masks.tp_dst |= 0xFFFF      => 0xFFFF
    
    return NULL  (不匹配)
  
  match = NULL
  match->priority <= hard_pri? => true (NULL 被视为优先级低)
  continue  (继续下一个 subtable)

wc 当前状态:
  wc->masks.nw_src = 0xFFFFFF00  ⭐
  wc->masks.tp_dst = 0xFFFF      ⭐
  其他 = 0
```

#### 迭代 2: Subtable B

```c
PVECTOR_FOR_EACH_PRIORITY 第 2 次迭代:
  subtable = Subtable B (priority 50)
  hard_pri + 1 = INT_MIN + 1 (仍然允许检查)
  
  调用: match = find_match_wc(Subtable B, ..., wc)
  
  在 find_match_wc() 内部:
    // Staged lookup 检查
    检查 nw_dst: 192.168.1.0 vs 192.168.1.0 => 匹配第一字段
    检查 tp_src: 12345 vs 443 => 不匹配
    
    // 累积 wildcards (到同一个 wc)
    wc->masks.nw_dst |= 0xFFFFFF00  => 0xFFFFFF00  ⭐ 新字段
    wc->masks.tp_src |= 0xFFFF      => 0xFFFF      ⭐ 新字段
    
    return NULL  (不匹配)
  
  match = NULL
  continue

wc 当前状态:
  wc->masks.nw_src = 0xFFFFFF00  (保留)
  wc->masks.nw_dst = 0xFFFFFF00  ⭐ 新增
  wc->masks.tp_src = 0xFFFF      ⭐ 新增
  wc->masks.tp_dst = 0xFFFF      (保留)
  其他 = 0
```

#### 迭代 3: Subtable C

```c
PVECTOR_FOR_EACH_PRIORITY 第 3 次迭代:
  subtable = Subtable C (priority 10)
  hard_pri + 1 = INT_MIN + 1 (仍然允许检查)
  
  调用: match = find_match_wc(Subtable C, ..., wc)
  
  在 find_match_wc() 内部:
    // Staged lookup 检查
    检查 dl_type: 0x0800 vs 0x0800 => 匹配！
    
    // 累积 wildcards
    wc->masks.dl_type |= 0xFFFF  => 0xFFFF  ⭐ 新字段
    
    return match  (找到匹配)
  
  match = <指向 Subtable C 中的规则>
  match->priority = 10
  match->priority <= hard_pri? => false (10 > INT_MIN)
  
  // 更新 hard 和 hard_pri
  hard = match
  hard_pri = 10
  
  继续迭代（但可能没有更多 subtable 或都被优先级剪枝）

wc 最终状态:
  wc->masks.dl_type = 0xFFFF     ⭐ 新增
  wc->masks.nw_src  = 0xFFFFFF00 (保留)
  wc->masks.nw_dst  = 0xFFFFFF00 (保留)
  wc->masks.tp_src  = 0xFFFF     (保留)
  wc->masks.tp_dst  = 0xFFFF     (保留)
  其他 = 0
```

---

## 阶段 6: Staged Lookup

### 代码位置
`lib/classifier.c: find_match_wc()`

### 详细代码

```c
static const struct cls_match *
find_match_wc(const struct cls_subtable *subtable, ovs_version_t version,
              const struct flow *flow, 
              struct trie_ctx *trie_ctx,
              uint32_t n_tries, struct flow_wildcards *wc)
{
    // 快速路径：如果不需要 wildcards
    if (OVS_UNLIKELY(!wc)) {
        return find_match(subtable, version, flow,
                          flow_hash_in_minimask(flow, &subtable->mask, 0));
    }
    
    uint32_t basis = 0, hash;
    const struct cls_match *rule = NULL;
    struct flowmap stages_map = FLOWMAP_EMPTY_INITIALIZER;  // ⭐ 追踪检查的 stages
    unsigned int mask_offset = 0;
    uint32_t i;
    
    // ⭐ Staged lookup 主循环
    for (i = 0; i < subtable->n_indices; i++) {
        // Trie 优化检查
        if (check_tries(trie_ctx, n_tries, subtable->trie_plen,
                        subtable->index_maps[i], flow, wc)) {
            // Trie 表明可以跳过
            goto no_match;
        }
        
        // ⭐ 累积已检查的 stage
        stages_map = flowmap_or(stages_map, subtable->index_maps[i]);
        
        // 计算该 stage 的 hash
        hash = flow_hash_in_minimask_range(flow, &subtable->mask,
                                           subtable->index_maps[i],
                                           &mask_offset, &basis);
        
        // ⭐ 检查该 stage 的 hash 索引
        if (!ccmap_find(&subtable->indices[i], hash)) {
            // 该 stage 没找到，说明不可能匹配
            goto no_match;
        }
    }
    
    // 所有 stage 都通过，检查最后的完整匹配
    stages_map = flowmap_or(stages_map, subtable->index_maps[i]);
    hash = flow_hash_in_minimask_range(flow, &subtable->mask,
                                       subtable->index_maps[i],
                                       &mask_offset, &basis);
    rule = find_match(subtable, version, flow, hash);
    
    if (rule) {
        // ⭐ 匹配成功：展开所有字段
        flow_wildcards_fold_minimask(wc, &subtable->mask);
        return rule;
    }
    
no_match:
    // ⭐ 匹配失败：只展开检查过的 stages
    flow_wildcards_fold_minimask_in_map(wc, &subtable->mask, stages_map);
    return NULL;
}
```

### Staged Lookup 示例

假设 Subtable 有 3 个 stages:

```
Subtable A:
  Stage 0: nw_src (index_maps[0] = {bit 6})
  Stage 1: tp_dst (index_maps[1] = {bit 9})
  Final:   完整匹配

Input flow:
  nw_src = 172.16.0.100
  tp_dst = 8080

Subtable A mask:
  nw_src = 255.255.255.0
  tp_dst = 0xFFFF
```

#### 执行流程

```c
i = 0 (Stage 0):
  stages_map = EMPTY
  hash = hash(172.16.0.0, stage 0)
  ccmap_find(indices[0], hash) => 找到
  stages_map |= index_maps[0]  => stages_map = {bit 6}  ⭐

i = 1 (Stage 1):
  hash = hash(8080, stage 1)
  ccmap_find(indices[1], hash) => 未找到！
  goto no_match
  
no_match:
  // 只检查了 stage 0 和部分 stage 1
  stages_map = {bit 6}  (只包含 stage 0 的字段)
  
  // ⭐ 只展开检查过的字段
  flow_wildcards_fold_minimask_in_map(wc, &subtable->mask, stages_map)
  
  // 结果：只有 nw_src 被 unwildcard，tp_dst 保持 wildcard
  wc->masks.nw_src |= 255.255.255.0  ⭐
  wc->masks.tp_dst 保持 0 (未检查完整，保持 wildcard)
```

**关键优化**:
- 如果在早期 stage 失败，后续字段不需要 unwildcard
- 这样生成的 megaflow 更宽（更多 wildcard）
- 提高 megaflow 复用率

---

## 阶段 7: Wildcards 累积

### 代码位置
`lib/classifier-private.h` 和 `lib/flow.h`

### 核心操作

```c
// lib/classifier-private.h
static inline void
flow_wildcards_fold_minimask(struct flow_wildcards *wc,
                              const struct minimask *mask)
{
    // 展开所有字段
    flow_union_with_miniflow(&wc->masks, &mask->masks);
}

static inline void
flow_wildcards_fold_minimask_in_map(struct flow_wildcards *wc,
                                    const struct minimask *mask,
                                    struct flowmap fmap)
{
    // 只展开指定 flowmap 中的字段
    flow_union_with_miniflow_subset(&wc->masks, &mask->masks, fmap);
}
```

### 按位 OR 实现

```c
// lib/flow.h
static inline void
flow_union_with_miniflow_subset(struct flow *dst, 
                                const struct miniflow *src,
                                struct flowmap subset)
{
    // dst = wc->masks (要修改的目标)
    // src = subtable->mask.masks (要累积的源)
    // subset = 要累积的字段范围
    
    uint64_t *dst_u64 = (uint64_t *) dst;
    const uint64_t *p = miniflow_get_values(src);
    
    FLOWMAP_FOR_EACH_MAP (map, subset) {
        MAP_FOR_EACH_INDEX (idx, map) {
            // ⭐ 核心操作：按位 OR
            dst_u64[idx] |= *p++;
        }
    }
}
```

### 详细示例

```c
// 初始状态
wc->masks.nw_src = 0x00000000

// Subtable A 累积
mask_A.nw_src = 0xFFFFFF00
wc->masks.nw_src |= 0xFFFFFF00
wc->masks.nw_src = 0x00000000 | 0xFFFFFF00 = 0xFFFFFF00  ⭐

// Subtable B (不涉及 nw_src)
// wc->masks.nw_src 保持 0xFFFFFF00 (不变)

// Subtable C (假设有更精确的 nw_src mask)
mask_C.nw_src = 0xFFFFFFFF
wc->masks.nw_src |= 0xFFFFFFFF
wc->masks.nw_src = 0xFFFFFF00 | 0xFFFFFFFF = 0xFFFFFFFF  ⭐

// 最终结果
wc->masks.nw_src = 0xFFFFFFFF  (最精确的 mask)
```

### 按位 OR 的数学特性

```
性质 1: 幂等性
  A | A = A
  多次累积同一个 mask 不会改变结果

性质 2: 可交换性
  A | B = B | A
  累积顺序不影响最终结果（虽然 OVS 按优先级顺序）

性质 3: 可结合性
  (A | B) | C = A | (B | C)
  可以逐步累积

性质 4: 单调性
  A | B >= A  (bit 位上)
  累积只会增加 1 的数量，永远不会减少
```

---

## 阶段 8: Megaflow 安装

### 代码位置
`ofproto/ofproto-dpif-upcall.c: ukey_create_from_upcall()`

### 代码

```c
static struct udpif_key *
ukey_create_from_upcall(struct upcall *upcall, struct flow_wildcards *wc)
{
    struct odputil_keybuf keystub, maskstub;
    struct ofpbuf keybuf, maskbuf;
    bool megaflow;
    
    // ⭐ 使用 wc 创建 ODP key parameters
    struct odp_flow_key_parms odp_parms = {
        .flow = upcall->flow,
        .mask = wc ? &wc->masks : NULL,  // ⭐ wc->masks 被用作 mask
    };
    
    // ... 创建 flow key ...
    
    // ⭐ 从 wc->masks 创建 mask key
    atomic_read_relaxed(&enable_megaflows, &megaflow);
    ofpbuf_use_stack(&maskbuf, &maskstub, sizeof maskstub);
    if (megaflow && wc) {
        odp_parms.key_buf = &keybuf;
        odp_flow_key_from_mask(&odp_parms, &maskbuf);
    }
    
    // 创建 ukey (userspace key)
    return ukey_create__(keybuf.data, keybuf.size, 
                        maskbuf.data, maskbuf.size,
                        true, upcall->ufid, upcall->pmd_id,
                        &upcall->put_actions, upcall->reval_seq, 0,
                        upcall->have_recirc_ref ? upcall->recirc->id : 0,
                        &upcall->xout);
}
```

### Megaflow 格式转换

```c
// wc->masks (struct flow 格式)
wc->masks = {
    .dl_type = 0xFFFF,
    .nw_src  = 0xFFFFFF00,
    .nw_dst  = 0xFFFFFF00,
    .tp_src  = 0xFFFF,
    .tp_dst  = 0xFFFF,
}

// ↓ odp_flow_key_from_mask() 转换

// ODP mask key (netlink 格式)
OVS_KEY_ATTR_ETHERNET: 
  eth_dst = ff:ff:ff:ff:ff:ff (未被任何 subtable 检查，默认全 F)
  eth_src = ff:ff:ff:ff:ff:ff

OVS_KEY_ATTR_ETHERTYPE: 0xFFFF  ⭐ 来自 wc->masks.dl_type

OVS_KEY_ATTR_IPV4:
  ipv4_src = 255.255.255.0      ⭐ 来自 wc->masks.nw_src
  ipv4_dst = 255.255.255.0      ⭐ 来自 wc->masks.nw_dst

OVS_KEY_ATTR_TCP:
  tcp_src = 0xFFFF              ⭐ 来自 wc->masks.tp_src
  tcp_dst = 0xFFFF              ⭐ 来自 wc->masks.tp_dst
```

### 安装到 Datapath

```c
// 最终 megaflow
Megaflow:
  Match (flow + mask):
    dl_type = 0x0800 & 0xFFFF = 0x0800
    nw_src  = 172.16.0.100 & 255.255.255.0 = 172.16.0.0
    nw_dst  = 192.168.1.50 & 255.255.255.0 = 192.168.1.0
    tp_src  = 12345 & 0xFFFF = 12345
    tp_dst  = 8080 & 0xFFFF = 8080
  
  Actions:
    来自 xlate_actions() 的 xout
```

---

## 完整示例追踪

### 场景设置

```yaml
Subtable A (priority 100):
  Mask: nw_src=255.255.255.0, tp_dst=0xFFFF
  Rule: src=10.0.0.0/24, dport=80 => output:1

Subtable B (priority 50):
  Mask: nw_dst=255.255.255.0, tp_src=0xFFFF
  Rule: dst=192.168.1.0/24, sport=443 => output:2

Subtable C (priority 10):
  Mask: dl_type=0xFFFF
  Rule: eth_type=IPv4 => output:3

Input Packet:
  dl_type = 0x0800 (IPv4)
  nw_src  = 172.16.0.100
  nw_dst  = 192.168.1.50
  tp_src  = 12345
  tp_dst  = 8080
```

### 完整 wc 变化追踪

```c
════════════════════════════════════════════════════════════════
时间点 0: 数据包到达 kernel datapath
════════════════════════════════════════════════════════════════
- Kernel 查找 megaflow: 未命中
- 触发 upcall 到 userspace

════════════════════════════════════════════════════════════════
时间点 1: recv_upcalls() 接收 upcall
════════════════════════════════════════════════════════════════
- 解析数据包，提取 flow
- flow = {dl_type=0x0800, nw_src=172.16.0.100, ...}

════════════════════════════════════════════════════════════════
时间点 2: process_upcall() 分发处理
════════════════════════════════════════════════════════════════
- upcall->type = MISS_UPCALL
- 调用 upcall_xlate()

════════════════════════════════════════════════════════════════
时间点 3: upcall_xlate() 开始
════════════════════════════════════════════════════════════════
struct flow_wildcards wc;  // 栈上分配

wc->masks = {
    .dl_type = 0x0000,    // wildcard
    .nw_src  = 0x00000000,// wildcard
    .nw_dst  = 0x00000000,// wildcard
    .tp_src  = 0x0000,    // wildcard
    .tp_dst  = 0x0000,    // wildcard
    ... (所有字段都是 0)
}

════════════════════════════════════════════════════════════════
时间点 4: xlate_actions(&xin, &xout)
════════════════════════════════════════════════════════════════
xin.wc = &wc  // 传入 wc 指针

// xlate 过程中调用 classifier_lookup()

════════════════════════════════════════════════════════════════
时间点 5: classifier_lookup(cls, version, flow, &wc, ...)
════════════════════════════════════════════════════════════════
// wc 指针传入 classifier

════════════════════════════════════════════════════════════════
时间点 6: 遍历 Subtable A (priority 100)
════════════════════════════════════════════════════════════════
调用: find_match_wc(Subtable A, ..., wc)

Subtable A 检查:
  nw_src: 172.16.0.0 != 10.0.0.0  => 不匹配
  
Wildcards 累积:
  wc->masks.nw_src |= 0xFFFFFF00
  wc->masks.tp_dst |= 0xFFFF

返回: NULL

wc->masks 当前状态:
  .nw_src  = 0xFFFFFF00  ⭐ 新增
  .tp_dst  = 0xFFFF      ⭐ 新增
  .dl_type = 0x0000      (未变)
  .nw_dst  = 0x00000000  (未变)
  .tp_src  = 0x0000      (未变)

════════════════════════════════════════════════════════════════
时间点 7: 遍历 Subtable B (priority 50)
════════════════════════════════════════════════════════════════
调用: find_match_wc(Subtable B, ..., wc)

Subtable B 检查:
  nw_dst: 192.168.1.0 == 192.168.1.0  => 匹配
  tp_src: 12345 != 443  => 不匹配
  
Wildcards 累积 (到同一个 wc):
  wc->masks.nw_dst |= 0xFFFFFF00
  wc->masks.tp_src |= 0xFFFF

返回: NULL

wc->masks 当前状态:
  .nw_src  = 0xFFFFFF00  (保留)
  .nw_dst  = 0xFFFFFF00  ⭐ 新增
  .tp_src  = 0xFFFF      ⭐ 新增
  .tp_dst  = 0xFFFF      (保留)
  .dl_type = 0x0000      (未变)

════════════════════════════════════════════════════════════════
时间点 8: 遍历 Subtable C (priority 10)
════════════════════════════════════════════════════════════════
调用: find_match_wc(Subtable C, ..., wc)

Subtable C 检查:
  dl_type: 0x0800 == 0x0800  => 匹配！
  
Wildcards 累积:
  wc->masks.dl_type |= 0xFFFF

返回: <匹配的规则>

wc->masks 当前状态:
  .dl_type = 0xFFFF      ⭐ 新增
  .nw_src  = 0xFFFFFF00  (保留)
  .nw_dst  = 0xFFFFFF00  (保留)
  .tp_src  = 0xFFFF      (保留)
  .tp_dst  = 0xFFFF      (保留)

════════════════════════════════════════════════════════════════
时间点 9: classifier_lookup__() 返回
════════════════════════════════════════════════════════════════
返回: Subtable C 的规则 (priority=10, action=output:3)

wc->masks 最终状态 (在 classifier 中):
  .dl_type = 0xFFFF
  .nw_src  = 0xFFFFFF00
  .nw_dst  = 0xFFFFFF00
  .tp_src  = 0xFFFF
  .tp_dst  = 0xFFFF
  (其他字段 = 0)

════════════════════════════════════════════════════════════════
时间点 10: xlate_actions() 完成
════════════════════════════════════════════════════════════════
xout->actions = output:3  (来自 Subtable C)

wc 仍然是同一个实例，包含累积的 wildcards

════════════════════════════════════════════════════════════════
时间点 11: ukey_create_from_upcall(upcall, &wc)
════════════════════════════════════════════════════════════════
使用 wc->masks 创建 megaflow mask

ODP mask key:
  OVS_KEY_ATTR_ETHERTYPE: 0xFFFF
  OVS_KEY_ATTR_IPV4:
    ipv4_src = 255.255.255.0
    ipv4_dst = 255.255.255.0
  OVS_KEY_ATTR_TCP:
    tcp_src = 0xFFFF
    tcp_dst = 0xFFFF

════════════════════════════════════════════════════════════════
时间点 12: dpif_operate(DPIF_OP_FLOW_PUT)
════════════════════════════════════════════════════════════════
安装 megaflow 到 kernel datapath:

Megaflow:
  Match:
    dl_type = 0x0800 (exact)
    nw_src  = 172.16.0.0/24  (来自 Subtable A 检查)
    nw_dst  = 192.168.1.0/24 (来自 Subtable B 检查)
    tp_src  = 12345 (exact, 来自 Subtable B 检查)
    tp_dst  = 8080 (exact, 来自 Subtable A 检查)
    其他字段 = wildcard
  
  Actions:
    output:3

════════════════════════════════════════════════════════════════
完成：wc 生命周期结束
════════════════════════════════════════════════════════════════
- wc 是栈上分配的局部变量
- 函数返回后自动销毁
- megaflow 已经安装到 kernel，包含了 wc 的所有信息
```

---

## 内存布局变化

### wc 在内存中的表示

```c
// struct flow_wildcards 在内存中的布局
struct flow_wildcards {
    struct flow masks;  // 240 bytes
};

// wc 在栈上的内存地址（示例）
wc 地址: 0x7ffc12345000

内存布局:
0x7ffc12345000: dl_dst (6 bytes)
0x7ffc12345006: dl_src (6 bytes)
0x7ffc1234500C: dl_type (2 bytes)
0x7ffc1234500E: pad1 (2 bytes)
0x7ffc12345010: vlans (16 bytes)
0x7ffc12345020: mpls_lse (16 bytes)
0x7ffc12345030: nw_src (4 bytes)   ⭐
0x7ffc12345034: nw_dst (4 bytes)   ⭐
...
0x7ffc12345xxx: tp_src (2 bytes)   ⭐
0x7ffc12345xxx: tp_dst (2 bytes)   ⭐
...
```

### 字段累积的内存操作

```c
// Subtable A 累积
// 地址 0x7ffc12345030 (nw_src)
之前: 0x00 0x00 0x00 0x00
操作: |= 0xFF 0xFF 0xFF 0x00
之后: 0xFF 0xFF 0xFF 0x00  ⭐

// 地址 0x7ffc12345xxx (tp_dst)
之前: 0x00 0x00
操作: |= 0xFF 0xFF
之后: 0xFF 0xFF  ⭐

// Subtable B 累积
// 地址 0x7ffc12345034 (nw_dst)
之前: 0x00 0x00 0x00 0x00
操作: |= 0xFF 0xFF 0xFF 0x00
之后: 0xFF 0xFF 0xFF 0x00  ⭐

// 地址 0x7ffc12345xxx (tp_src)
之前: 0x00 0x00
操作: |= 0xFF 0xFF
之后: 0xFF 0xFF  ⭐

// 地址 0x7ffc12345030 (nw_src) - 保持不变
保持: 0xFF 0xFF 0xFF 0x00  (之前的值)

// Subtable C 累积
// 地址 0x7ffc1234500C (dl_type)
之前: 0x00 0x00
操作: |= 0xFF 0xFF
之后: 0xFF 0xFF  ⭐

// 其他字段 - 全部保持之前的值
```

---

## 关键要点总结

### 1. wc 的生命周期

```
创建 → 初始化 → 传递 → 累积 → 使用 → 销毁
  ↑       ↑       ↑      ↑      ↑       ↑
  栈上   全 0    指针   多次OR  生成    自动
```

### 2. 单一实例原则

- ✅ 整个查找过程只有**一个 wc 实例**
- ✅ 通过**指针传递**，不复制
- ✅ 所有修改都作用于**同一块内存**

### 3. 累积机制

- ✅ 使用**按位 OR** (`|=`) 累积
- ✅ 永远**不覆盖**之前的值
- ✅ 只会**增加**精确度（更多 1 bits）
- ✅ **单调递增**（bit 位上）

### 4. 保存所有检查

- ✅ 匹配的 subtable: 记录
- ✅ 不匹配的 subtable: **也记录**
- ✅ 早期失败的 stage: 只记录检查过的
- ✅ 优先级剪枝跳过的: **不记录**

### 5. 最终用途

- wc->masks 转换为 ODP netlink mask
- 与 flow 结合生成 megaflow match
- 安装到 kernel datapath
- 后续数据包直接在 kernel 匹配

---

## 调试技巧

### 打印 wc 状态

```c
// 在关键点插入调试代码
static void print_wc(const struct flow_wildcards *wc, const char *label) {
    VLOG_INFO("%s: nw_src=%08x, nw_dst=%08x, tp_src=%04x, tp_dst=%04x, dl_type=%04x",
              label,
              ntohl(wc->masks.nw_src),
              ntohl(wc->masks.nw_dst),
              ntohs(wc->masks.tp_src),
              ntohs(wc->masks.tp_dst),
              ntohs(wc->masks.dl_type));
}

// 使用
print_wc(wc, "Before classifier_lookup");
classifier_lookup(cls, version, flow, wc, NULL);
print_wc(wc, "After classifier_lookup");
```

### GDB 断点

```bash
# 设置断点
(gdb) break find_match_wc
(gdb) commands
> print subtable->mask
> print *wc
> continue
> end

# 运行并观察 wc 变化
(gdb) run
```

---

**文档创建时间**: 2025-10-19  
**作者**: GitHub Copilot  
**版本**: 1.0
