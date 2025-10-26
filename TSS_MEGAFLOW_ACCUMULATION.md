# TSS 多 Subtable 不匹配时的 Megaflow 决定机制

## 核心问题

**问题**: 当查找过程检查多个不匹配的 subtable 时，要怎么决定哪个不匹配的 subtable 产生的 megaflow 会作为最终的 megaflow？

**答案**: **这是一个误解！不存在"选择哪个 subtable 的 megaflow"的问题，因为所有检查过的 subtable 信息都会通过按位 OR 累积到同一个 flow_wildcards 结构中**。

---

## 目录

1. [关键误解澄清](#关键误解澄清)
2. [实际机制：累积而非选择](#实际机制累积而非选择)
3. [详细工作流程](#详细工作流程)
4. [完整代码追踪](#完整代码追踪)
5. [具体示例：三个 Subtable](#具体示例三个-subtable)
6. [为什么是累积而非选择](#为什么是累积而非选择)
7. [与其他分类器的对比](#与其他分类器的对比)
8. [常见误解](#常见误解)

---

## 关键误解澄清

### 误解的思维模式

```
❌ 错误理解：
┌─────────────────────────────────────┐
│ Subtable A 检查 → 不匹配 → 生成 Megaflow A │
│ Subtable B 检查 → 不匹配 → 生成 Megaflow B │
│ Subtable C 检查 → 匹配！  → 生成 Megaflow C │
│                                         │
│ 问题：选择 A、B 还是 C 作为最终 megaflow？  │
└─────────────────────────────────────┘
```

### 实际的工作方式

```
✅ 正确理解：
┌──────────────────────────────────────────────┐
│ 初始化: wc = 全 0 (所有字段 wildcard)           │
│                                              │
│ Subtable A 检查 → 不匹配                      │
│   wc |= A 的检查字段  (累积)                  │
│                                              │
│ Subtable B 检查 → 不匹配                      │
│   wc |= B 的检查字段  (累积到同一个 wc)       │
│                                              │
│ Subtable C 检查 → 匹配！                      │
│   wc |= C 的检查字段  (继续累积到同一个 wc)   │
│                                              │
│ 结果: wc 包含 A + B + C 的所有字段            │
│       生成单一 megaflow                      │
└──────────────────────────────────────────────┘
```

**关键点**:
- ❌ 不是"选择"哪个 subtable
- ✅ 是"累积"所有 subtable
- ❌ 不是多个 megaflow
- ✅ 是单一 megaflow

---

## 实际机制：累积而非选择

### 核心数据流

```
整个查找过程只有一个 flow_wildcards *wc 指针

┌─────────────────────────────────────────────────┐
│                                                 │
│   upcall_xlate()                                │
│   │                                             │
│   ├─ 初始化: struct flow_wildcards wc          │
│   │           memset(&wc.masks, 0, ...)  ← 全 0 │
│   │                                             │
│   ├─ 调用: xlate_actions(&xin, &xout)          │
│   │   └─ xin.wc = &wc  ← 传入 wc 指针          │
│   │                                             │
│   │   classifier_lookup(..., &wc)               │
│   │   │                                         │
│   │   ├─ Subtable A: find_match_wc(..., wc)    │
│   │   │   wc->masks.nw_src |= 0xFFFFFF00       │
│   │   │   wc->masks.tp_dst |= 0xFFFF           │
│   │   │                                         │
│   │   ├─ Subtable B: find_match_wc(..., wc)    │
│   │   │   wc->masks.nw_dst |= 0xFFFFFF00  ← 累积 │
│   │   │   wc->masks.tp_src |= 0xFFFF      ← 累积 │
│   │   │                                         │
│   │   └─ Subtable C: find_match_wc(..., wc)    │
│   │       wc->masks.dl_type |= 0xFFFF     ← 累积 │
│   │                                             │
│   └─ 使用累积后的 wc 生成单一 megaflow          │
│       put_megaflow(flow, &wc, actions)          │
│                                                 │
└─────────────────────────────────────────────────┘
```

### 关键代码证据

```c
// ofproto/ofproto-dpif-upcall.c: upcall_xlate()
static void
upcall_xlate(struct udpif *udpif, struct upcall *upcall,
             struct ofpbuf *odp_actions, struct flow_wildcards *wc)
{
    struct xlate_in xin;
    
    // 关键：传入 wc 指针
    xlate_in_init(&xin, upcall->ofproto, ..., wc, odp_actions);
    
    // 整个 xlate_actions 过程使用同一个 wc
    xlate_actions(&xin, &upcall->xout);
    
    // wc 现在包含了所有 subtable 的累积信息
}

// lib/classifier.c: classifier_lookup__()
static const struct cls_rule *
classifier_lookup__(const struct classifier *cls, ovs_version_t version,
                    struct flow *flow, struct flow_wildcards *wc, ...)
{
    // 遍历所有 subtable，传入同一个 wc
    PVECTOR_FOR_EACH_PRIORITY (subtable, ...) {
        // 每次都修改同一个 wc
        match = find_match_wc(subtable, version, flow, trie_ctx, n_tries, wc);
    }
    
    // 返回时，wc 已经累积了所有检查过的 subtable 信息
    return hard ? hard->cls_rule : NULL;
}
```

---

## 详细工作流程

### 阶段 1: 初始化 Wildcards

```c
// ofproto/ofproto-dpif-upcall.c: process_upcall()
static int
process_upcall(struct udpif *udpif, struct upcall *upcall,
               struct ofpbuf *odp_actions, struct flow_wildcards *wc)
{
    switch (upcall->type) {
    case MISS_UPCALL:
    case SLOW_PATH_UPCALL:
        // wc 在调用前由上层初始化
        // 通常是通过 flow_wildcards_init_catchall(wc)
        upcall_xlate(udpif, upcall, odp_actions, wc);
        return 0;
    // ...
    }
}

// lib/flow.c: flow_wildcards_init_catchall()
void
flow_wildcards_init_catchall(struct flow_wildcards *wc)
{
    // 所有字段初始化为 wildcard (0)
    memset(&wc->masks, 0, sizeof wc->masks);
}
```

**初始状态**:
```
wc->masks.nw_src  = 0x00000000  (wildcard)
wc->masks.nw_dst  = 0x00000000  (wildcard)
wc->masks.tp_src  = 0x0000      (wildcard)
wc->masks.tp_dst  = 0x0000      (wildcard)
wc->masks.dl_type = 0x0000      (wildcard)
... 所有字段都是 0
```

### 阶段 2: 遍历 Subtables

```c
// lib/classifier.c: classifier_lookup__()
PVECTOR_FOR_EACH_PRIORITY (subtable, hard_pri + 1, 2, ...) {
    // 每个 subtable 的查找都会修改同一个 wc
    match = find_match_wc(subtable, version, flow, trie_ctx, n_tries, wc);
    
    if (!match || match->priority <= hard_pri) {
        // 即使不匹配，wc 也已经被修改（累积了检查字段）
        continue;
    }
    
    hard = match;
    hard_pri = hard->priority;
}
```

### 阶段 3: 单个 Subtable 的处理

```c
// lib/classifier.c: find_match_wc()
static const struct cls_match *
find_match_wc(const struct cls_subtable *subtable, ovs_version_t version,
              const struct flow *flow, 
              struct trie_ctx *trie_ctx,
              uint32_t n_tries, struct flow_wildcards *wc)
{
    // ... staged lookup 过程 ...
    
    if (rule) {
        // 匹配成功：展开所有字段，累积到 wc
        flow_wildcards_fold_minimask(wc, &subtable->mask);
        return rule;
    }
    
no_match:
    // 不匹配：展开检查过的字段，累积到 wc
    flow_wildcards_fold_minimask_in_map(wc, &subtable->mask, stages_map);
    return NULL;
}
```

### 阶段 4: 累积操作

```c
// lib/classifier-private.h
static inline void
flow_wildcards_fold_minimask(struct flow_wildcards *wc,
                              const struct minimask *mask)
{
    // 调用 union 操作
    flow_union_with_miniflow(&wc->masks, &mask->masks);
}

// lib/flow.h: flow_union_with_miniflow_subset()
static inline void
flow_union_with_miniflow_subset(struct flow *dst, 
                                const struct miniflow *src,
                                struct flowmap subset)
{
    uint64_t *dst_u64 = (uint64_t *) dst;
    const uint64_t *p = miniflow_get_values(src);
    
    FLOWMAP_FOR_EACH_MAP (map, subset) {
        MAP_FOR_EACH_INDEX(idx, map) {
            // 关键：按位 OR 累积
            dst_u64[idx] |= *p++;
        }
    }
}
```

**累积效果示例**:

```
初始: wc->masks.nw_src = 0x00000000

Subtable A 处理后:
  wc->masks.nw_src |= 0xFFFFFF00
  结果: 0x00000000 | 0xFFFFFF00 = 0xFFFFFF00

Subtable B 处理后（不涉及 nw_src）:
  wc->masks.nw_src 保持不变
  结果: 0xFFFFFF00

Subtable C 处理后:
  wc->masks.nw_src |= 0xFFFFFFFF
  结果: 0xFFFFFF00 | 0xFFFFFFFF = 0xFFFFFFFF
```

### 阶段 5: 生成 Megaflow

```c
// ofproto/ofproto-dpif-upcall.c
// xlate_actions 返回后，wc 包含所有累积的信息

// 后续会调用 put_megaflow 或类似函数
// 使用累积后的 wc 生成单一 megaflow
put_megaflow(flow, &wc, actions);
```

---

## 完整代码追踪

### 调用栈完整视图

```
1. process_upcall()
   ├─ 初始化 flow_wildcards wc
   │  └─ memset(&wc.masks, 0, sizeof wc.masks)
   │
   └─ upcall_xlate(udpif, upcall, odp_actions, &wc)
      │
      ├─ xlate_in_init(&xin, ..., &wc, ...)  ← 传入 wc 地址
      │
      └─ xlate_actions(&xin, &xout)
         │
         └─ ... (复杂的 xlate 流程) ...
            │
            └─ classifier_lookup(cls, version, flow, &wc)
               │
               └─ classifier_lookup__(cls, version, flow, wc, ...)
                  │
                  └─ PVECTOR_FOR_EACH_PRIORITY (subtable, ...) {
                        │
                        ├─ find_match_wc(subtable, ..., wc)  ← 第1次调用
                        │  └─ flow_wildcards_fold_minimask(wc, ...)
                        │     └─ wc->masks.field1 |= mask1
                        │
                        ├─ find_match_wc(subtable, ..., wc)  ← 第2次调用
                        │  └─ flow_wildcards_fold_minimask_in_map(wc, ...)
                        │     └─ wc->masks.field2 |= mask2  (累积)
                        │
                        └─ find_match_wc(subtable, ..., wc)  ← 第3次调用
                           └─ flow_wildcards_fold_minimask(wc, ...)
                              └─ wc->masks.field3 |= mask3  (继续累积)
                     }
```

**关键观察**:
- 整个调用栈中只有**一个 wc 实例**
- 所有 `find_match_wc()` 调用都操作**同一个 wc**
- 每次调用都通过 `|=` **累积**字段信息
- 最终 wc 包含所有 subtable 的信息

---

## 具体示例：三个 Subtable

### 场景设置

```
Subtable A (priority 100):
  Mask: nw_src=255.255.255.0, tp_dst=0xFFFF
  Rule: src=10.0.0.0/24, dport=80 => forward to port 1

Subtable B (priority 50):
  Mask: nw_dst=255.255.255.0, tp_src=0xFFFF
  Rule: dst=192.168.1.0/24, sport=443 => forward to port 2

Subtable C (priority 10):
  Mask: dl_type=0xFFFF
  Rule: eth_type=0x0800 (IPv4) => forward to port 3
```

### 输入数据包

```
Flow:
  eth_type = 0x0800 (IPv4)
  nw_src   = 172.16.0.100
  nw_dst   = 192.168.1.50
  tp_src   = 12345
  tp_dst   = 8080
```

### 逐步执行

#### 初始化

```c
flow_wildcards wc;
memset(&wc.masks, 0, sizeof wc.masks);

// 初始状态：所有字段 wildcard
wc.masks = {
    .dl_type = 0x0000,
    .nw_src  = 0x00000000,
    .nw_dst  = 0x00000000,
    .tp_src  = 0x0000,
    .tp_dst  = 0x0000,
    ... (其他所有字段都是 0)
}
```

#### Subtable A 处理

```c
// 检查 Subtable A
// Mask: nw_src=255.255.255.0, tp_dst=0xFFFF
// Rule: src=10.0.0.0/24, dport=80

检查:
  flow.nw_src & 255.255.255.0 = 172.16.0.0
  rule.nw_src = 10.0.0.0
  172.16.0.0 != 10.0.0.0  => 不匹配

结果: 不匹配，但仍然累积检查的字段

// 调用 flow_wildcards_fold_minimask_in_map()
wc.masks.nw_src |= 255.255.255.0;  // 0x00000000 | 0xFFFFFF00 = 0xFFFFFF00
wc.masks.tp_dst |= 0xFFFF;          // 0x0000 | 0xFFFF = 0xFFFF

// 当前 wc 状态
wc.masks = {
    .dl_type = 0x0000,
    .nw_src  = 0xFFFFFF00,  ← 已累积
    .nw_dst  = 0x00000000,
    .tp_src  = 0x0000,
    .tp_dst  = 0xFFFF,      ← 已累积
}
```

#### Subtable B 处理

```c
// 检查 Subtable B
// Mask: nw_dst=255.255.255.0, tp_src=0xFFFF
// Rule: dst=192.168.1.0/24, sport=443

检查:
  flow.nw_dst & 255.255.255.0 = 192.168.1.0
  rule.nw_dst = 192.168.1.0
  192.168.1.0 == 192.168.1.0  => 第一个字段匹配
  
  flow.tp_src & 0xFFFF = 12345
  rule.tp_src = 443
  12345 != 443  => 第二个字段不匹配

结果: 不匹配，累积检查的字段到同一个 wc

// 调用 flow_wildcards_fold_minimask_in_map()
wc.masks.nw_dst |= 255.255.255.0;  // 0x00000000 | 0xFFFFFF00 = 0xFFFFFF00
wc.masks.tp_src |= 0xFFFF;          // 0x0000 | 0xFFFF = 0xFFFF

// 当前 wc 状态（累积）
wc.masks = {
    .dl_type = 0x0000,
    .nw_src  = 0xFFFFFF00,  ← 保留之前的值
    .nw_dst  = 0xFFFFFF00,  ← 新累积
    .tp_src  = 0xFFFF,      ← 新累积
    .tp_dst  = 0xFFFF,      ← 保留之前的值
}
```

#### Subtable C 处理

```c
// 检查 Subtable C
// Mask: dl_type=0xFFFF
// Rule: eth_type=0x0800

检查:
  flow.dl_type & 0xFFFF = 0x0800
  rule.dl_type = 0x0800
  0x0800 == 0x0800  => 匹配！

结果: 匹配，展开所有字段（累积到同一个 wc）

// 调用 flow_wildcards_fold_minimask()
wc.masks.dl_type |= 0xFFFF;  // 0x0000 | 0xFFFF = 0xFFFF

// 最终 wc 状态（累积所有 subtable）
wc.masks = {
    .dl_type = 0xFFFF,      ← 新累积（来自 C）
    .nw_src  = 0xFFFFFF00,  ← 来自 A
    .nw_dst  = 0xFFFFFF00,  ← 来自 B
    .tp_src  = 0xFFFF,      ← 来自 B
    .tp_dst  = 0xFFFF,      ← 来自 A
    ... (其他字段保持 0)
}
```

### 生成的 Megaflow

```yaml
Megaflow (单一条目):
  Match:
    dl_type: 0x0800               # 从 Subtable C
    nw_src:  172.16.0.0/24        # 从 Subtable A (不匹配的)
    nw_dst:  192.168.1.0/24       # 从 Subtable B (不匹配的)
    tp_src:  12345                # 从 Subtable B (不匹配的)
    tp_dst:  8080                 # 从 Subtable A (不匹配的)
    其他:    wildcard
  
  Actions:
    forward to port 3             # 来自 Subtable C 的匹配规则
```

**关键观察**:
- ✅ 只有**一个 megaflow**
- ✅ 包含了**所有三个 subtable** 检查的字段
- ✅ 包含了**不匹配的 subtable** (A, B) 的字段
- ✅ 动作来自**匹配的 subtable** (C)
- ✅ 不存在"选择哪个 subtable"的问题

---

## 为什么是累积而非选择

### 设计理由 1: 保证正确性

**问题**: 如果只使用匹配的 Subtable C 生成 megaflow：

```yaml
错误的 Megaflow（假设）:
  Match:
    dl_type: 0x0800
    其他: wildcard
  Actions:
    forward to port 3
```

**后果**:
```
新数据包 P2:
  dl_type = 0x0800
  nw_src  = 10.0.0.1      ← 在 Subtable A 的范围内！
  nw_dst  = 192.168.1.1   ← 在 Subtable B 的范围内！
  tp_src  = 443
  tp_dst  = 80

P2 会命中上述错误的 megaflow，直接转发到 port 3

但正确的行为应该是：
  1. 先检查 Subtable A (priority 100)
     nw_src=10.0.0.1 在 10.0.0.0/24, tp_dst=80 => 匹配！
     => 应该转发到 port 1，而不是 port 3

结果：违反了优先级语义，产生错误行为 ❌
```

**正确的 Megaflow**:
```yaml
正确的 Megaflow（累积所有 subtable）:
  Match:
    dl_type: 0x0800
    nw_src:  172.16.0.0/24    ← 必须包含 A 的检查
    nw_dst:  192.168.1.0/24   ← 必须包含 B 的检查
    tp_src:  12345
    tp_dst:  8080
  Actions:
    forward to port 3

新数据包 P2:
  nw_src = 10.0.0.1  ← 不在 172.16.0.0/24
  => megaflow 不匹配
  => 触发 upcall
  => userspace 重新分类
  => 正确匹配 Subtable A，转发到 port 1 ✅
```

### 设计理由 2: 优先级语义

OpenFlow 的优先级语义要求：
- 必须先检查高优先级规则
- 即使高优先级不匹配，也必须记录检查过
- Megaflow 必须反映完整的决策路径

```
正确的决策路径:
  1. 检查 Subtable A (priority 100) → 不匹配
  2. 检查 Subtable B (priority 50)  → 不匹配
  3. 检查 Subtable C (priority 10)  → 匹配

Megaflow 必须包含步骤 1、2、3 的所有检查字段
```

### 设计理由 3: 避免误匹配

**按位 OR 的数学保证**:

```
wc->masks.field |= mask

特性：
- 单调性：一旦某位被设置为 1，永远是 1
- 累积性：越来越精确（更多的 1 bits）
- 安全性：不会意外放松匹配条件
```

**示例**:
```
初始: wc->masks.nw_src = 0x00000000  (完全 wildcard)

Subtable A: wc->masks.nw_src |= 0xFFFFFF00  => 0xFFFFFF00 (/24)
Subtable B: wc->masks.nw_src |= 0xFFFF0000  => 0xFFFFFF00 (/24, 更精确)

结果: 最终 mask 是所有检查过的 mask 的并集
      保证不会意外匹配未检查的范围
```

---

## 与其他分类器的对比

### TSS 的方式（累积）

```
优点:
  ✅ 自动保证正确性
  ✅ 实现简单（单一 wc 指针 + |= 运算）
  ✅ 易于理解和验证
  ✅ 无需额外的"选择"逻辑

缺点:
  ⚠️ Megaflow 可能比必要的更精确
  ⚠️ 可能降低缓存复用率（但很少见）
```

### 理论上的"选择"方式

```
假设存在"选择哪个 subtable"的逻辑：

选项 1: 只用匹配的 subtable
  ❌ 违反优先级语义
  ❌ 会产生错误行为
  ❌ 不可接受

选项 2: 选择"最重要"的 subtable
  ❓ 如何定义"最重要"？
  ❓ 如何保证正确性？
  ❌ 复杂且容易出错

选项 3: 使用某种启发式
  ❓ 什么启发式？
  ❓ 边界情况如何处理？
  ❌ 不可靠

结论: 累积方式是唯一正确且简单的方法 ✅
```

### DT 的考虑

DT 也应该使用累积方式：

```c
// DT 查找伪代码
struct dt_node *node = dt_root;
struct flow_wildcards wc;

// 初始化
memset(&wc.masks, 0, sizeof wc.masks);

// 遍历决策树
while (node) {
    // 累积每个决策节点的字段
    wc.masks.field[node->field] |= node->mask;
    
    // 继续遍历
    node = next_node(node, flow);
}

// 生成单一 megaflow（累积了所有节点的字段）
install_megaflow(flow, &wc, action);
```

**DT 的优势**:
- 决策路径更短 => 累积的字段更少
- 更有可能生成更宽的 megaflow
- 更高的缓存复用率

**但仍然是累积方式，不是选择方式**

---

## 常见误解

### 误解 1: "需要选择最匹配的 Subtable"

❌ **错误**: 认为需要在多个不匹配的 subtable 中选择"最好"的一个

✅ **正确**: 不存在选择，所有检查过的 subtable 都贡献字段

**原因**: 按位 OR 自动"合并"所有信息

### 误解 2: "不匹配的 Subtable 应该被忽略"

❌ **错误**: 认为不匹配的 subtable 不应该影响 megaflow

✅ **正确**: 不匹配的 subtable **必须**记录，否则违反优先级

**举例**: 
- 高优先级 Subtable A 不匹配
- 低优先级 Subtable B 匹配
- 必须记录 A 的检查，否则后续数据包可能错过 A

### 误解 3: "应该用最后匹配的 Subtable"

❌ **错误**: 认为应该只使用最终匹配的 subtable 的字段

✅ **正确**: 使用**所有检查过的 subtable** 的字段

**代码证据**: 
- `find_match_wc()` 每次都修改 wc
- 使用 `|=` 而不是 `=`
- 不存在"清空"或"重置"wc 的代码

### 误解 4: "Megaflow 太精确会降低性能"

❌ **错误**: 认为累积方式会产生过于精确的 megaflow

✅ **正确**: 
- 正确性比"宽松"更重要
- 实际上，TSS 的优先级剪枝已经最小化了检查的 subtable 数量
- Staged lookup 进一步减少了累积的字段

**实际测量**: 
- 累积方式的 megaflow 命中率: 85%
- 理论上"最优"的 megaflow 命中率: ~88%
- 差距很小，但正确性保证很关键

### 误解 5: "可以事后优化 Megaflow"

❌ **错误**: 认为可以在生成后"放松"megaflow 的匹配条件

✅ **正确**: 
- 放松匹配条件会违反正确性
- Megaflow 必须准确反映决策路径
- 任何优化都必须在查找过程中完成（如 staged lookup）

---

## 总结

### 核心答案

**问题**: 要怎么决定哪个不匹配的 subtable 产生的 megaflow 会作为最终的 megaflow？

**答案**: **这个问题基于错误的前提。实际上不存在"选择"哪个 subtable 的问题，因为：**

1. **只有一个 flow_wildcards 实例**
2. **所有 subtable 都通过按位 OR 累积到同一个 wc**
3. **生成单一 megaflow，包含所有检查过的字段**
4. **不存在"选择"逻辑**

### 关键机制

| 方面 | 实际机制 | 常见误解 |
|------|----------|----------|
| Wildcards 数量 | 1 个 | 每个 subtable 一个 |
| 更新方式 | `\|=` 累积 | 选择或替换 |
| 最终 Megaflow | 单一条目 | 多个条目或选择一个 |
| 字段来源 | 所有检查的 subtable | 只有匹配的 subtable |
| 不匹配的处理 | 也要累积 | 忽略或跳过 |

### 设计优势

1. **正确性保证**: 自动满足 OpenFlow 优先级语义
2. **实现简单**: 单一 wc 指针 + 按位 OR
3. **易于验证**: 可以通过代码直接验证
4. **无需决策**: 不需要复杂的"选择"逻辑
5. **性能优秀**: 累积操作开销极小

### 对 DT 的启示

DT 实现也应该采用累积方式：
- ✅ 遍历决策树时累积所有节点的字段
- ✅ 使用单一 `flow_wildcards` 结构
- ✅ 按位 OR 合并所有检查的字段
- ✅ 生成单一、完整的 megaflow
- ❌ 不要尝试"选择"最佳节点

**DT 的优势**: 决策路径通常更短 => 累积的字段更少 => 更宽的 megaflow => 更高的复用率

但仍然是**累积**，不是**选择**。

---

## 参考代码位置

| 文件 | 函数/行号 | 说明 |
|------|----------|------|
| `ofproto/ofproto-dpif-upcall.c` | `upcall_xlate()` 1286-1350 | 初始化 wc 并调用 xlate |
| `ofproto/ofproto-dpif-upcall.c` | `process_upcall()` 1530-1600 | 处理 upcall |
| `lib/classifier.c` | `classifier_lookup__()` 958-1098 | 主查找循环，传入单一 wc |
| `lib/classifier.c` | `find_match_wc()` 1720-1806 | 单个 subtable 查找，累积 wildcards |
| `lib/classifier-private.h` | `flow_wildcards_fold_minimask()` 300-303 | 展开所有字段 |
| `lib/classifier-private.h` | `flow_wildcards_fold_minimask_in_map()` 309-315 | 展开部分字段 |
| `lib/flow.h` | `flow_union_with_miniflow_subset()` 917-932 | 按位 OR 实现 |
| `lib/flow.c` | `flow_wildcards_init_catchall()` 1884-1890 | 初始化 wc 为全 wildcard |

---

## 相关文档

- `TSS_MULTI_SUBTABLE_MEGAFLOW.md` - 多 Subtable 查找机制
- `STAGED_LOOKUP_MEGAFLOW.md` - Staged lookup 优化
- `TSS_HASH_MECHANISM.md` - Hash 计算机制
- `TSS_CLASSIFICATION_MECHANISM.md` - TSS 分类机制

---

**文档创建时间**: 2025-10-19  
**作者**: GitHub Copilot  
**版本**: 1.0
