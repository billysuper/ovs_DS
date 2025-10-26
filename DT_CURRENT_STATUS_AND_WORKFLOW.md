# Decision Tree (DT) 当前状态与运作流程

**文档时间**: 2025-10-19 23:20  
**状态**: 核心功能已实现，全量建树测试遇到数据结构问题

---

## 📊 整体架构

```
┌─────────────────────────────────────────────┐
│         Decision Tree Classifier            │
├─────────────────────────────────────────────┤
│                                             │
│  ┌─────────────┐      ┌─────────────┐     │
│  │   dt_init   │      │dt_build_tree│     │
│  │  (空树初始化)│      │ (全量建树)   │     │
│  └─────┬───────┘      └──────┬──────┘     │
│        │                     │            │
│        v                     v            │
│  ┌─────────────────────────────────────┐  │
│  │     Decision Tree Structure         │  │
│  │  ┌──────────────────────────────┐   │  │
│  │  │   Root Node (RCU-protected)  │   │  │
│  │  └───────────┬──────────────────┘   │  │
│  │              │                       │  │
│  │      ┌───────┴────────┐             │  │
│  │      v                v             │  │
│  │  Internal          Leaf             │  │
│  │  Nodes           Nodes              │  │
│  └─────────────────────────────────────┘  │
│                                             │
│  ┌──────────────────────────────────────┐ │
│  │  Operations:                         │ │
│  │  • dt_insert_rule (COW增量插入)      │ │
│  │  • dt_remove_rule (标记删除)         │ │
│  │  • dt_lookup_simple (查找)           │ │
│  │  • dt_destroy (销毁释放)             │ │
│  └──────────────────────────────────────┘ │
└─────────────────────────────────────────────┘
```

---

## 🗂️ 文件结构

### 核心文件

```
lib/dt-classifier.h       (147 lines) - 数据结构定义、API声明
lib/dt-classifier.c       (882 lines) - 实现逻辑
tests/test-dt-bulk.c      (293 lines) - 全量建树测试（当前有问题）
```

### 文档文件

```
DT_ALGORITHM_EXPLAINED.md          - 算法详解
DT_ALGORITHM_VISUAL.md             - 可视化说明
DT_INTEGRATION_DESIGN.md           - 集成设计方案
DT_NEXT_STEPS.md                   - 后续步骤
DT_VERSION_INTEGRATION.md          - Version机制集成
DT_INITIALIZATION_STRATEGY.md      - 初始化策略
DT_INCREMENTAL_VS_BULK_ANALYSIS.md - 增量 vs 全量分析
DT_BULK_BUILD_TEST_PLAN.md         - 全量测试计划
DT_BULK_BUILD_QUICK_START.md       - 快速开始指南
```

---

## 🏗️ 数据结构详解

### 1. Decision Tree 主结构

```c
struct decision_tree {
    OVSRCU_TYPE(struct dt_node *) root;  // RCU保护的根节点
    int n_rules;                          // 规则总数
    int n_internal_nodes;                 // 内部节点数
    int n_leaf_nodes;                     // 叶子节点数
    int max_depth;                        // 最大深度
    int n_deferred_ops;                   // 延迟操作数
};
```

**特点**:
- 使用 RCU (Read-Copy-Update) 保护根节点
- 支持无锁并发读取
- 统计信息用于性能监控

### 2. 节点类型

#### 内部节点 (Internal Node)

```c
struct dt_internal_node {
    const struct mf_field *field;    // 测试的字段 (如 IPV4_SRC)
    enum dt_test_type test_type;     // 测试类型
    
    union {
        struct {
            ovs_be32 value;          // 精确匹配值
        } exact;
        
        struct {
            ovs_be32 prefix;         // 前缀值
            unsigned int plen;       // 前缀长度
        } prefix;
        
        struct {
            ovs_be32 min;            // 范围最小值
            ovs_be32 max;            // 范围最大值
        } range;
    } test;
    
    OVSRCU_TYPE(struct dt_node *) left;   // 左子树 (False分支)
    OVSRCU_TYPE(struct dt_node *) right;  // 右子树 (True分支)
};
```

**测试类型**:
```c
enum dt_test_type {
    DT_TEST_EXACT,   // 精确匹配: field == value
    DT_TEST_PREFIX,  // 前缀匹配: field & mask == prefix
    DT_TEST_RANGE    // 范围匹配: min <= field <= max (未实现)
};
```

#### 叶子节点 (Leaf Node) - 当前版本

```c
struct dt_leaf_node {
    const struct cls_rule **rules;  // 规则指针数组 (刚修改)
    size_t n_rules;                 // 规则数量
    size_t capacity;                // 数组容量
    uint32_t leaf_id;               // 叶子节点ID
};
```

**⚠️ 重要变更**:
- **旧版本**: 使用 `struct rculist rules` (链表)
- **新版本**: 改用 `const struct cls_rule **rules` (指针数组)
- **原因**: 避免链表节点共享导致的无限循环问题
- **状态**: 刚修改，需要更新所有相关代码

#### 叶子节点 - 原始版本 (已废弃)

```c
struct dt_leaf_node {
    struct rculist rules;              // 规则链表
    struct rculist inherited_rules;    // 继承的通配规则
    struct minimask required_mask;     // 到达此叶子的必需掩码
    size_t n_rules;                    // 直接规则数
    size_t n_inherited;                // 继承规则数
    uint32_t leaf_id;                  // 叶子ID
};
```

### 3. 节点联合体

```c
struct dt_node {
    enum dt_node_type type;  // DT_NODE_INTERNAL 或 DT_NODE_LEAF
    
    union {
        struct dt_internal_node internal;
        struct dt_leaf_node leaf;
    };
};
```

---

## 🔄 核心操作流程

### 1. 初始化流程 (`dt_init`)

```
dt_init(dt)
    │
    ├─ ovsrcu_set_hidden(&dt->root, NULL)  // 空树
    ├─ dt->n_rules = 0
    ├─ dt->n_internal_nodes = 0
    ├─ dt->n_leaf_nodes = 0
    ├─ dt->max_depth = 0
    └─ dt->n_deferred_ops = 0
```

**特点**:
- 创建空树，符合 OVS 的初始化模式
- 与 TSS (Tuple Space Search) 一致
- 规则通过 `dt_insert_rule` 逐个添加

### 2. 增量插入流程 (`dt_insert_rule`)

```
dt_insert_rule(dt, rule, version)
    │
    ├─ 检查根节点是否为空
    │   │
    │   ├─ 空 → 创建包含该规则的叶子节点
    │   │        return true
    │   │
    │   └─ 非空 → 继续
    │
    ├─ 从根节点开始遍历
    │   │
    │   └─ dt_insert_rule_simple(&root, rule)
    │
    └─ 更新统计: dt->n_rules++
```

**`dt_insert_rule_simple` 详细流程**:

```
dt_insert_rule_simple(root, rule)
    │
    ├─ 如果 *root 是叶子节点:
    │   │
    │   ├─ COW: 复制叶子节点 → new_leaf
    │   ├─ 检查规则是否已存在
    │   │   └─ 存在 → 替换; 不存在 → 添加
    │   ├─ 检查是否需要分裂
    │   │   │
    │   │   ├─ n_rules <= MAX_LEAF_SIZE (10)
    │   │   │   └─ 不分裂，直接添加
    │   │   │
    │   │   └─ n_rules > MAX_LEAF_SIZE
    │   │       └─ 分裂叶子节点
    │   │           ├─ 选择分裂字段 (dt_select_split_field)
    │   │           ├─ 确定分裂值 (dt_find_split_value)
    │   │           ├─ 分配规则到左右子树
    │   │           └─ 创建内部节点
    │   │
    │   └─ RCU更新: ovsrcu_set(root, new_leaf)
    │
    └─ 如果 *root 是内部节点:
        │
        ├─ COW: 复制路径上的节点 (dt_node_copy)
        ├─ 评估测试条件确定方向
        │   ├─ True → 递归插入到右子树
        │   └─ False → 递归插入到左子树
        └─ RCU更新父节点指针
```

**Copy-on-Write (COW) 机制**:

```
为什么需要 COW?
┌────────────────────────────────────────┐
│  并发读者可能正在使用旧版本的树        │
│  → 不能直接修改节点                    │
│  → 复制修改路径上的所有节点            │
│  → 用新节点替换旧节点 (RCU)           │
│  → 旧节点在 grace period 后释放       │
└────────────────────────────────────────┘

实现:
1. dt_node_copy(old_node) → new_node
2. 在 new_node 上修改
3. ovsrcu_set(&parent->child, new_node)
4. ovsrcu_postpone(free, old_node)
```

### 3. 查找流程 (`dt_lookup_simple`)

```
dt_lookup_simple(dt, flow)
    │
    ├─ node = ovsrcu_get(struct dt_node *, &dt->root)
    │
    └─ dt_lookup_node(node, flow)
        │
        ├─ 如果 node 是叶子:
        │   │
        │   └─ 遍历 leaf->rules[]
        │       │
        │       └─ miniflow_equal_flow_in_minimask()
        │           ├─ 匹配 → 返回规则
        │           └─ 不匹配 → 继续
        │
        └─ 如果 node 是内部节点:
            │
            ├─ 从 flow 中提取测试字段值
            ├─ 评估测试条件
            │   ├─ True → 递归查找右子树
            │   └─ False → 递归查找左子树
            │
            └─ 返回匹配的规则 (或 NULL)
```

**字段测试逻辑**:

```c
// 精确匹配
if (test_type == DT_TEST_EXACT) {
    matches = (flow_value == test_value);
}

// 前缀匹配
if (test_type == DT_TEST_PREFIX) {
    ovs_be32 mask = htonl(~0u << (32 - plen));
    matches = ((flow_value & mask) == (prefix & mask));
}

// 范围匹配 (未实现)
if (test_type == DT_TEST_RANGE) {
    matches = (flow_value >= min && flow_value <= max);
}
```

### 4. 删除流程 (`dt_remove_rule`)

```
dt_remove_rule(dt, rule)
    │
    ├─ 标记规则为删除
    │   └─ cls_match_make_invisible_in_version(rule, version)
    │
    ├─ 增加延迟操作计数
    │   └─ dt->n_deferred_ops++
    │
    └─ 检查是否需要重建
        │
        ├─ n_deferred_ops > REBUILD_THRESHOLD
        │   └─ 触发树重建 (未完全实现)
        │
        └─ 否则延迟处理
```

**设计思想**:
- **惰性删除**: 不立即从树中移除
- **版本控制**: 使用 OVS Version 机制标记
- **延迟重建**: 累积到阈值后批量重建

### 5. 全量建树流程 (`dt_build_tree`)

```
dt_build_tree(rules, n_rules, max_leaf_size)
    │
    ├─ 基础情况: n_rules <= max_leaf_size
    │   │
    │   ├─ 创建叶子节点
    │   ├─ 分配规则数组: rules[n_rules]
    │   ├─ 复制规则指针 (不复制节点!)
    │   │   └─ RCULIST_FOR_EACH(rule, ...)
    │   │       └─ leaf->rules[i++] = rule
    │   └─ 返回叶子节点
    │
    └─ 递归情况: n_rules > max_leaf_size
        │
        ├─ 1. 选择分裂字段
        │   └─ dt_select_split_field(rules, n_rules)
        │       │
        │       ├─ 候选字段:
        │       │   • MFF_IN_PORT
        │       │   • MFF_ETH_TYPE
        │       │   • MFF_IPV4_SRC ✓ (最常用)
        │       │   • MFF_IPV4_DST
        │       │   • MFF_IP_PROTO
        │       │   • MFF_TCP_SRC/DST
        │       │   • MFF_UDP_SRC/DST
        │       │
        │       ├─ 对每个候选字段:
        │       │   └─ 统计有多少规则关注此字段
        │       │       (检查 mask 非零)
        │       │
        │       └─ 返回关注度最高的字段
        │
        ├─ 2. 确定分裂值
        │   └─ dt_find_split_value(field, rules, ...)
        │       │
        │       ├─ 收集该字段的所有值
        │       │   └─ values[] = {v1, v2, ..., vN}
        │       │
        │       └─ 选择中位数
        │           └─ split_value = values[N/2]
        │
        ├─ 3. 分割规则
        │   │
        │   ├─ 初始化两个新链表
        │   │   ├─ left_rules
        │   │   └─ right_rules
        │   │
        │   └─ RCULIST_FOR_EACH(rule, ...)
        │       │
        │       ├─ 从 rule 获取字段值
        │       ├─ 评估: value >= split_value?
        │       │   ├─ True → right_rules
        │       │   └─ False → left_rules
        │       │
        │       └─ 将规则指针添加到对应链表
        │
        ├─ 4. 递归构建子树
        │   ├─ left_subtree = dt_build_tree(left_rules, ...)
        │   └─ right_subtree = dt_build_tree(right_rules, ...)
        │
        ├─ 5. 创建内部节点
        │   ├─ internal->field = split_field
        │   ├─ internal->test_type = DT_TEST_EXACT
        │   ├─ internal->test.exact.value = split_value
        │   ├─ internal->left = left_subtree
        │   └─ internal->right = right_subtree
        │
        └─ 返回内部节点
```

**⚠️ 当前问题**:

```
问题: 链表节点共享导致无限循环
────────────────────────────────────

原因:
1. cls_rule 的 node 字段已在 rules 链表中
2. 试图将同一个 node 添加到 left_rules 或 right_rules
3. 破坏了链表结构，形成循环引用
4. RCULIST_FOR_EACH 无法终止 → 遍历 68000+ 次

临时修复:
- 改用规则指针数组: const struct cls_rule **rules
- 只复制指针，不操作链表节点

影响范围:
- 需要修改 ~20+ 处代码
- dt_node_copy, dt_remove_rule, dt_lookup_node 等
```

---

## 🔑 关键算法

### 1. 字段选择算法

```c
/* 启发式: 选择最多规则关注的字段 */
dt_select_split_field(rules, n_rules)
    │
    ├─ 对每个候选字段:
    │   │
    │   ├─ field_count = 0
    │   │
    │   └─ 遍历所有规则:
    │       │
    │       ├─ minimatch_expand(&rule->match, &match)
    │       ├─ mf_get(field, &match, &value, &mask)
    │       │
    │       └─ if (!is_all_zeros(&mask, field->n_bytes))
    │           └─ field_count++
    │
    └─ 返回 field_count 最大的字段
```

**改进空间**:
- [ ] 实现信息增益 (Information Gain) 计算
- [ ] 考虑规则分布的均匀性
- [ ] 动态调整候选字段列表

### 2. 分裂值选择算法

```c
/* 当前实现: 中位数法 */
dt_find_split_value(field, rules, ...)
    │
    ├─ 收集所有值
    │   └─ values[] = {v0, v1, ..., v(n-1)}
    │
    ├─ 选择中位数
    │   └─ split_value = values[n/2]
    │
    └─ 测试类型 = DT_TEST_EXACT
```

**改进空间**:
- [ ] 对 IP 地址使用前缀匹配
- [ ] 对端口使用范围匹配
- [ ] 考虑规则优先级

### 3. COW 路径复制算法

```c
/* 复制从根到插入点的路径 */
dt_insert_rule_simple(root, rule)
    │
    ├─ 当前节点是叶子:
    │   └─ new_leaf = dt_node_copy(*root)
    │       └─ 修改 new_leaf
    │           └─ *root = new_leaf
    │
    └─ 当前节点是内部节点:
        │
        ├─ new_internal = dt_node_copy(*root)
        ├─ 递归插入到子树
        │   └─ dt_insert_rule_simple(&new_internal->child, rule)
        │
        └─ *root = new_internal
```

**性能特点**:
- 时间复杂度: O(depth) = O(log N)
- 空间复杂度: O(depth) 个新节点
- 并发安全: 读者可以继续使用旧树

---

## 🔬 Version 机制集成

### OVS Version 系统

```c
struct versions {
    ovs_version_t add_version;              // 规则创建版本
    ATOMIC(ovs_version_t) remove_version;   // 规则删除版本
};

/* 可见性检查 */
bool visible = (add_version <= query_version < remove_version);
```

### DT 中的应用

```c
/* 查找时需要检查版本 */
dt_lookup(dt, flow, version) {
    const struct cls_rule *rule = dt_lookup_simple(dt, flow);
    
    if (rule && cls_match_visible_in_version(rule, version)) {
        return rule;  // 可见
    }
    
    return NULL;  // 不可见或不存在
}

/* 删除时标记版本 */
dt_remove_rule(dt, rule, version) {
    cls_match_make_invisible_in_version(rule, version);
    dt->n_deferred_ops++;
}
```

**集成状态**:
- ✅ 数据结构已支持 (cls_rule 包含 versions)
- ✅ 查找 API 已有 version 参数
- ⚠️  需要在所有查找路径添加可见性检查
- ⚠️  需要实现延迟删除和树重建

---

## 📈 性能特征

### 理论复杂度

| 操作 | 时间复杂度 | 空间复杂度 | 备注 |
|------|-----------|-----------|------|
| 初始化 | O(1) | O(1) | 创建空树 |
| 插入 | O(log N) | O(log N) | COW 路径复制 |
| 删除 | O(1) | O(1) | 惰性删除 |
| 查找 | O(log N) | O(1) | 树遍历 |
| 全量建树 | O(N log N) | O(N) | 递归分割 |

### 实际性能估算

```
规则数量      树深度      插入时间    查找时间
─────────────────────────────────────────────
10           3-4        ~1 μs      ~0.5 μs
100          6-8        ~2 μs      ~1 μs
1000         10-12      ~3 μs      ~2 μs
10000        13-17      ~5 μs      ~3 μs
```

### 内存使用

```c
/* 每个内部节点 */
sizeof(struct dt_node) ≈ 64 bytes

/* 每个叶子节点 (新版本) */
sizeof(struct dt_node) + 
sizeof(struct cls_rule *) * n_rules

/* 示例: 1000 条规则 */
- 内部节点: ~150 个 × 64 = 9.6 KB
- 叶子节点: ~151 个 × 64 = 9.7 KB
- 规则指针: 1000 × 8 = 8 KB
- 总计: ~27 KB
```

---

## ⚠️ 已知问题和限制

### 1. 全量建树链表问题 ❌

**状态**: 刚发现，正在修复中

**问题描述**:
```c
// ❌ 错误做法
RCULIST_FOR_EACH (rule, node, rules) {
    rculist_push_back(&leaf->rules, &rule->node);  
    // 破坏链表结构!
}
```

**影响**:
- 无法使用 `dt_build_tree` 进行全量建树测试
- 测试套件无法运行

**修复方案**:
- ✅ 已改用规则指针数组
- ⚠️  需要更新 ~20+ 处代码
- 预计时间: 2-3 小时

### 2. 字段提取未实现 ⚠️

**当前状态**:
```c
// dt_find_split_value 中
values[n_values++] = htonl(n_values);  // 使用虚拟值!
```

**影响**:
- 分裂值不是基于实际数据
- 树结构不是最优的

**修复方案**:
```c
// 需要实现
minimatch_expand(&rule->match, &match);
mf_get(field, &match, &value, &mask);
values[n_values++] = value;  // 使用真实值
```

### 3. 规则分割逻辑简化 ⚠️

**当前实现**:
```c
ovs_be32 test_value = htonl(rule_count);  // 虚拟值
bool goes_right = (test_value >= split_value);
```

**问题**:
- 不是基于规则的实际字段值
- 导致分割不准确

**修复方案**:
- 从 rule->match 中提取实际字段值
- 使用正确的测试逻辑

### 4. 版本检查不完整 ⚠️

**问题**:
- `dt_lookup_simple` 不检查版本
- 可能返回已删除的规则

**修复方案**:
```c
// 在叶子节点遍历时
for (i = 0; i < leaf->n_rules; i++) {
    rule = leaf->rules[i];
    
    if (!cls_match_visible_in_version(rule, version)) {
        continue;  // 跳过不可见的规则
    }
    
    if (match_rule(rule, flow)) {
        return rule;
    }
}
```

### 5. 延迟删除未实现 ⚠️

**当前状态**:
```c
dt_remove_rule(dt, rule) {
    // 只标记，不真正删除
    dt->n_deferred_ops++;
}
```

**缺失功能**:
- 没有触发树重建的逻辑
- 没有真正从树中移除规则
- 可能导致内存泄漏

---

## ✅ 已完成功能

### 核心数据结构 ✓
- [x] `struct decision_tree` - 主结构
- [x] `struct dt_node` - 节点联合体
- [x] `struct dt_internal_node` - 内部节点
- [x] `struct dt_leaf_node` - 叶子节点 (指针数组版本)

### 基础操作 ✓
- [x] `dt_init()` - 初始化空树
- [x] `dt_destroy()` - 销毁释放
- [x] `dt_lookup_simple()` - 简单查找
- [x] `dt_node_create_leaf()` - 创建叶子
- [x] `dt_node_create_internal()` - 创建内部节点
- [x] `dt_node_copy()` - COW 节点复制

### 增量操作 ✓
- [x] `dt_insert_rule()` - 入口函数
- [x] `dt_insert_rule_simple()` - 递归插入
- [x] COW 路径复制机制
- [x] 叶子节点分裂逻辑

### 全量建树 ⚠️ (部分完成)
- [x] `dt_build_tree()` - 递归构建框架
- [x] `dt_select_split_field()` - 字段选择
- [x] `dt_find_split_value()` - 分裂值选择
- [ ] 正确的规则分割 (使用虚拟值)
- [ ] 数据结构兼容性 (正在修复)

### 辅助功能 ✓
- [x] `dt_get_stats()` - 统计信息
- [x] `dt_print_tree()` - 可视化打印
- [x] RCU 保护机制

---

## 📝 集成到 Classifier 的准备度

### 已具备的条件 ✅

1. **API 兼容性**
   ```c
   // DT API 已设计为与 classifier API 兼容
   bool dt_insert_rule(struct decision_tree *dt, 
                       const struct cls_rule *rule,
                       ovs_version_t version);
   
   const struct cls_rule *dt_lookup(struct decision_tree *dt,
                                     const struct flow *flow,
                                     ovs_version_t version);
   ```

2. **数据结构独立性**
   - DT 使用自己的节点结构
   - 不修改 `cls_rule` 结构
   - 可以与 TSS 共存

3. **并发安全性**
   - RCU 保护读取
   - COW 保证写入安全
   - 符合 OVS 并发模型

### 需要完成的工作 ⚠️

1. **修复全量建树** (2-3 小时)
   - 更新所有使用 `leaf->rules` 的代码
   - 测试通过基础功能

2. **实现真实字段提取** (1-2 小时)
   - `dt_find_split_value` 使用实际值
   - 规则分割使用实际值

3. **完善版本检查** (1 小时)
   - 在查找路径添加可见性检查
   - 实现延迟删除和重建

4. **修改 classifier.h/c** (2-4 小时)
   - 添加后端类型枚举
   - 添加 DT 实例到 classifier
   - 实现函数分发逻辑

5. **集成测试** (2-3 小时)
   - 运行现有测试套件
   - 对比 TSS 和 DT 结果
   - 性能基准测试

**总估算时间**: 8-13 小时

---

## 🎯 下一步行动建议

### 选项 1: 明天继续修复全量建树 (推荐)
- 休息后思路更清晰
- 系统性修复所有相关代码
- 完整测试后再集成

### 选项 2: 跳过全量建树，直接集成
- 全量建树只是测试工具
- 增量插入是核心功能
- 可以先集成到 classifier
- 后续优化全量建树

### 选项 3: 创建增量测试
- 避开全量建树问题
- 测试 `dt_insert_rule` 逐个插入
- 验证核心功能正确性

---

## 📊 代码统计

```
文件                      行数    功能
──────────────────────────────────────────────
dt-classifier.h           147     数据结构、API
dt-classifier.c           882     实现逻辑
test-dt-bulk.c            293     全量测试
──────────────────────────────────────────────
总计                      1322    代码行数

文档                      行数
──────────────────────────────────────────────
DT_*.md (9 个文件)        ~60000  设计文档
```

---

## 🔍 调试信息

### 当前调试输出示例

```
=== Test: DT Bulk Build Basic (10 rules) ===
Creating 10 test rules...
Building tree with dt_build_tree()...
  Rules list addr: 0x7ffe5d43b3a0
  Number of rules: 10
  Calling dt_build_tree...
[DT] dt_build_tree ENTER: depth=1, n_rules=10, max_leaf=10
[DT]   Base case: creating leaf node with 10 rules
[DT]   Collecting rule pointers...
[DT]     Rule 0
[DT]     Rule 1
[DT]     Rule 68609   ← 无限循环!
[DT]     Rule 68610
```

### 问题诊断

1. **进入基础情况** ✓
2. **创建叶子节点** ✓
3. **遍历链表** ❌ → 无限循环

**根本原因**: 链表节点被多个链表共享

---

## 📚 相关概念

### RCU (Read-Copy-Update)
- 允许多个读者并发访问
- 写者创建新版本
- 旧版本在 grace period 后释放

### COW (Copy-on-Write)
- 修改时复制路径
- 保留旧版本供读者使用
- 原子性地切换到新版本

### Version (MVCC)
- 多版本并发控制
- 每个规则有 add_version 和 remove_version
- 查询时检查版本可见性

---

## 🎓 学习要点

### DT 的核心思想
1. **空间换时间**: 预先构建决策树，加速查找
2. **分层递归**: 每层测试一个字段，缩小搜索空间
3. **增量维护**: 支持动态插入删除，无需重建
4. **并发友好**: RCU + COW 实现无锁读取

### 与 TSS 的对比
- **TSS**: Hash表 + 多个子表，平均 O(1)，最坏 O(N)
- **DT**: 二叉树，稳定 O(log N)
- **TSS**: 空间小，适合规则少
- **DT**: 空间大，适合规则多

---

**文档完成时间**: 2025-10-19 23:45  
**状态**: 详细总结完成，等待下一步决策
