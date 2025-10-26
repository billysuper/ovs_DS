# OVS Version 机制详解

## 核心问题

**问题**: OVS classifier 中的 `ovs_version_t version` 参数是做什么用的？

**答案**: **实现 MVCC (Multi-Version Concurrency Control)，支持无锁并发读写**。

---

## 目录

1. [基本概念](#基本概念)
2. [数据结构](#数据结构)
3. [Version 的生命周期](#version-的生命周期)
4. [并发场景分析](#并发场景分析)
5. [实际示例](#实际示例)
6. [性能优势](#性能优势)
7. [对 DT 的启示](#对-dt-的启示)

---

## 基本概念

### MVCC (Multi-Version Concurrency Control)

```
问题:
  - OVS 需要高性能并发访问
  - 读操作频繁（每个数据包查找）
  - 写操作偶尔（添加/删除规则）

传统方案 (锁):
  读写互斥 → 读操作被阻塞 → 性能低下

MVCC 方案:
  - 每个规则有多个版本
  - 读者读取特定版本
  - 写者创建新版本
  - 读写不互斥 ✅
```

### Version 的类型

```c
typedef uint64_t ovs_version_t;

特殊值:
  OVS_VERSION_MIN = 0                  // 默认版本
  OVS_VERSION_MAX = UINT64_MAX - 1     // 最大版本
  OVS_VERSION_NOT_REMOVED = UINT64_MAX // 规则未删除
```

---

## 数据结构

### 核心结构: `struct versions`

```c
// lib/versions.h
struct versions {
    ovs_version_t add_version;              // ⭐ 规则被添加的版本
    ATOMIC(ovs_version_t) remove_version;   // ⭐ 规则被删除的版本
};

宏:
#define VERSIONS_INITIALIZER(ADD, REMOVE) \
    (struct versions){ ADD, REMOVE }
```

### 可见性判断函数

```c
static inline bool
versions_visible_in_version(const struct versions *versions,
                            ovs_version_t version)
{
    ovs_version_t remove_version;
    
    atomic_read_relaxed(&versions->remove_version, &remove_version);
    
    // ⭐ 核心逻辑: add_version <= version < remove_version
    return versions->add_version <= version && version < remove_version;
}
```

**关键**: 规则在 `[add_version, remove_version)` 区间内可见。

---

## Version 的生命周期

### 阶段 1: 规则插入

```c
// lib/classifier.c: classifier_replace()
const struct cls_rule *
classifier_replace(struct classifier *cls, const struct cls_rule *rule,
                   ovs_version_t version,  // ⭐ 传入版本号
                   const struct cls_conjunction *conjs, size_t n_conjs)
{
    // 创建新的 cls_match，设置版本
    new = cls_match_alloc(rule, version, conjs, n_conjs);
    
    // ...
    
    // 使规则在指定版本可见
    cls_match_set_remove_version(new, OVS_VERSION_NOT_REMOVED);
}
```

#### `cls_match_alloc()` 实现

```c
// lib/classifier.c
static struct cls_match *
cls_match_alloc(const struct cls_rule *rule, ovs_version_t version,
                const struct cls_conjunction *conjs, size_t n_conjs)
{
    struct cls_match *cls_match = /* 分配内存 */;
    
    // ⭐ 初始化版本信息
    cls_match->versions = VERSIONS_INITIALIZER(version, version);
    //                                         ^^^^^^  ^^^^^^
    //                                         add     remove (临时值)
    
    // 后续会调用 cls_match_set_remove_version() 设置为 OVS_VERSION_NOT_REMOVED
    
    return cls_match;
}
```

**状态**:
```
初始: versions = {add: 10, remove: 10}     // 不可见
设置后: versions = {add: 10, remove: MAX}   // 从版本 10 开始可见
```

---

### 阶段 2: 规则查找

```c
// lib/classifier.c: classifier_lookup__()
static const struct cls_rule *
classifier_lookup__(const struct classifier *cls, 
                    ovs_version_t version,  // ⭐ 查找者的版本
                    struct flow *flow, struct flow_wildcards *wc,
                    bool allow_conjunctive_matches,
                    struct hmapx *conj_flows)
{
    // 遍历 subtable
    PVECTOR_FOR_EACH_PRIORITY (subtable, hard_pri + 1, 2, ...) {
        // 在 subtable 中查找
        match = find_match_wc(subtable, version, flow, trie_ctx, n_tries, wc);
        //                              ^^^^^^^
        //                              传递版本号
        
        if (!match || match->priority <= hard_pri) {
            continue;
        }
        
        // 找到匹配
        hard = match;
        hard_pri = hard->priority;
    }
}
```

#### `find_match()` 中的版本检查

```c
// lib/classifier.c
static inline const struct cls_match *
find_match(const struct cls_subtable *subtable, ovs_version_t version,
           const struct flow *flow, uint32_t hash)
{
    const struct cls_match *head, *rule;

    CMAP_FOR_EACH_WITH_HASH (head, cmap_node, hash, &subtable->rules) {
        if (miniflow_and_mask_matches_flow(&head->flow, &subtable->mask, flow)) {
            // 找到匹配的规则，遍历相同 hash 的规则列表
            CLS_MATCH_FOR_EACH (rule, head) {
                // ⭐ 关键: 检查规则是否在指定版本可见
                if (OVS_LIKELY(cls_match_visible_in_version(rule, version))) {
                    return rule;  // 返回第一个可见的规则
                }
            }
        }
    }

    return NULL;
}
```

#### `cls_match_visible_in_version()` 实现

```c
// lib/classifier-private.h
static inline bool
cls_match_visible_in_version(const struct cls_match *rule,
                             ovs_version_t version)
{
    return versions_visible_in_version(&rule->versions, version);
}

// lib/versions.h
static inline bool
versions_visible_in_version(const struct versions *versions,
                            ovs_version_t version)
{
    ovs_version_t remove_version;
    atomic_read_relaxed(&versions->remove_version, &remove_version);
    
    // ⭐ 核心判断
    return versions->add_version <= version && version < remove_version;
}
```

---

### 阶段 3: 规则删除

```c
// lib/classifier.c: cls_rule_make_invisible_in_version()
void
cls_rule_make_invisible_in_version(const struct cls_rule *rule,
                                   ovs_version_t remove_version)
{
    struct cls_match *cls_match = get_cls_match_protected(rule);

    // 断言: 删除版本必须 >= 添加版本
    ovs_assert(remove_version >= cls_match->versions.add_version);

    // ⭐ 设置删除版本
    cls_match_set_remove_version(cls_match, remove_version);
}
```

#### `cls_match_set_remove_version()` 实现

```c
// lib/classifier-private.h
static inline void
cls_match_set_remove_version(struct cls_match *rule, ovs_version_t version)
{
    versions_set_remove_version(&rule->versions, version);
}

// lib/versions.h
static inline void
versions_set_remove_version(struct versions *versions, ovs_version_t version)
{
    atomic_store_relaxed(&versions->remove_version, version);
}
```

**状态变化**:
```
删除前: versions = {add: 10, remove: MAX}   // 版本 10-MAX 可见
删除后: versions = {add: 10, remove: 50}    // 版本 10-49 可见，50+ 不可见
```

---

### 阶段 4: 恢复可见性

```c
// lib/classifier.c: cls_rule_restore_visibility()
void
cls_rule_restore_visibility(const struct cls_rule *rule)
{
    cls_match_set_remove_version(get_cls_match_protected(rule),
                                 OVS_VERSION_NOT_REMOVED);
}
```

**用途**: 撤销删除操作（例如事务回滚）。

---

## 并发场景分析

### 场景 1: 读写并发

```
时间轴:
  T0: 规则 R1 存在，versions = {add: 1, remove: MAX}
  
  T1: 读者 A 开始查找，使用 version = 10
  
  T2: 写者 W 删除 R1，设置 remove_version = 20
      新状态: versions = {add: 1, remove: 20}
  
  T3: 读者 A 继续查找
      检查 R1: 1 <= 10 < 20 ? ✅ 是
      → R1 对读者 A 仍然可见
  
  T4: 读者 B 开始查找，使用 version = 25
      检查 R1: 1 <= 25 < 20 ? ❌ 否
      → R1 对读者 B 不可见

结果:
  - 读者 A (旧版本) 看到 R1
  - 读者 B (新版本) 看不到 R1
  - 读写不互斥 ✅
```

### 场景 2: 规则替换

```
初始状态:
  规则 R1: versions = {add: 1, remove: MAX}
  
操作:
  1. 写者 W 插入新规则 R2 (版本 20)
     R2: versions = {add: 20, remove: MAX}
  
  2. 写者 W 标记 R1 为删除 (版本 20)
     R1: versions = {add: 1, remove: 20}

并发读取:
  读者 A (version=10):
    R1: 1 <= 10 < MAX ✅ 可见
    R2: 20 <= 10 ❌ 不可见
    → 看到 R1
  
  读者 B (version=25):
    R1: 1 <= 25 < 20 ❌ 不可见
    R2: 20 <= 25 < MAX ✅ 可见
    → 看到 R2

结果:
  - 旧读者看旧规则
  - 新读者看新规则
  - 平滑过渡 ✅
```

### 场景 3: 多个规则版本共存

```
规则链 (相同匹配条件，不同版本):
  R1: {add: 1,  remove: 10}  → action A
  R2: {add: 10, remove: 20}  → action B
  R3: {add: 20, remove: MAX} → action C

不同版本的读者:
  version=5:  看到 R1 (action A)
  version=15: 看到 R2 (action B)
  version=25: 看到 R3 (action C)

代码实现:
  CLS_MATCH_FOR_EACH (rule, head) {
      if (cls_match_visible_in_version(rule, version)) {
          return rule;  // ⭐ 返回第一个可见的
      }
  }
```

---

## 实际示例

### 示例 1: 简单的规则更新

#### 初始状态 (version=10)

```
Classifier:
  规则 R1: nw_src=10.0.0.0/8 → forward
  versions = {add: 1, remove: MAX}
```

#### 更新操作 (version=20)

```
1. 插入新规则 R2:
   nw_src=10.0.0.0/8 → drop
   versions = {add: 20, remove: MAX}

2. 标记 R1 删除:
   versions = {add: 1, remove: 20}
```

#### 并发查找

```c
// 旧版本查找 (version=15)
match = classifier_lookup(cls, version=15, flow, wc);

检查 R1: 1 <= 15 < 20 ✅ → 返回 R1 (forward)
检查 R2: 20 <= 15 ❌     → 不检查

结果: forward ✅

// 新版本查找 (version=25)
match = classifier_lookup(cls, version=25, flow, wc);

检查 R1: 1 <= 25 < 20 ❌ → 跳过
检查 R2: 20 <= 25 < MAX ✅ → 返回 R2 (drop)

结果: drop ✅
```

---

### 示例 2: 复杂的事务场景

#### 场景描述

```
事务操作 (version=30):
  1. 删除规则 R1
  2. 添加规则 R2
  3. 添加规则 R3
  4. 事务提交
```

#### 详细过程

```c
// T0: 初始状态
R1: {add: 1, remove: MAX}

// T1: 开始事务 (version=30)
version_bump = 30;

// T2: 删除 R1
cls_rule_make_invisible_in_version(R1, 30);
R1: {add: 1, remove: 30}  // 版本 30 之后不可见

// T3: 添加 R2
classifier_replace(cls, R2, version=30, ...);
R2: {add: 30, remove: MAX}

// T4: 添加 R3
classifier_replace(cls, R3, version=30, ...);
R3: {add: 30, remove: MAX}

// T5: 并发读取
读者 A (version=25):
  R1 可见 (1 <= 25 < 30)
  R2 不可见 (30 <= 25 ❌)
  R3 不可见 (30 <= 25 ❌)
  → 看到旧状态 (只有 R1)

读者 B (version=35):
  R1 不可见 (1 <= 35 < 30 ❌)
  R2 可见 (30 <= 35 < MAX)
  R3 可见 (30 <= 35 < MAX)
  → 看到新状态 (R2, R3)

// T6: 事务提交
// 更新全局版本号
global_version = 30;
```

**关键**: 读者看到的是**一致的快照**（要么全是旧的，要么全是新的）。

---

## 性能优势

### 1. 无锁读取

```
传统方案 (读写锁):
  读取时:
    1. 获取读锁 (10-50 ns)
    2. 查找规则 (500 ns)
    3. 释放读锁 (10-50 ns)
    总计: ~600 ns

MVCC 方案:
  读取时:
    1. 查找规则 (500 ns)  ← 无锁！
    总计: ~500 ns

加速: 20%
```

### 2. 写操作不阻塞读

```
传统方案:
  写操作时，所有读操作被阻塞
  写延迟: 1-10 ms
  影响: 数万次读操作

MVCC 方案:
  写操作时，读操作继续（读旧版本）
  写延迟: 对读无影响
  影响: 0 次读操作 ✅
```

### 3. 支持事务

```
MVCC 天然支持事务:
  - 所有修改使用同一版本号
  - 提交前对新读者不可见
  - 提交后原子可见
  - 回滚只需恢复版本号
```

---

## 对 DT 的启示

### DT 是否需要 Version？

```
取决于使用场景:

场景 A: DT 独立使用
  - 如果需要并发读写 → 需要 version
  - 如果只有单线程 → 不需要 version

场景 B: DT 集成到 OVS
  - ✅ 必须支持 version
  - 原因: OVS 已经使用 MVCC
  - 需要与现有机制兼容
```

### DT 实现建议

#### 方案 1: 完全兼容 OVS 的 version 机制

```c
struct dt_rule {
    struct versions versions;  // ⭐ 复用 OVS 的版本结构
    // ... 其他字段
};

const struct dt_rule *
dt_lookup(const struct decision_tree *dt, ovs_version_t version,
          const struct flow *flow, struct flow_wildcards *wc)
{
    struct dt_node *node = dt->root;
    
    while (node) {
        if (node->rule) {
            // ⭐ 检查版本可见性
            if (dt_rule_visible_in_version(node->rule, version)) {
                return node->rule;
            }
        }
        node = next_node(node, flow);
    }
    return NULL;
}
```

#### 方案 2: 简化版本（如果 DT 独立）

```c
// 如果 DT 不需要复杂的并发控制
struct dt_rule {
    bool active;  // 简单的活动标志
};

const struct dt_rule *
dt_lookup(const struct decision_tree *dt,
          const struct flow *flow, struct flow_wildcards *wc)
{
    struct dt_node *node = dt->root;
    
    while (node) {
        if (node->rule && node->rule->active) {
            return node->rule;
        }
        node = next_node(node, flow);
    }
    return NULL;
}
```

### 集成到 OVS 时的考虑

```c
// 统一的查找接口
const struct cls_rule *
classifier_lookup(const struct classifier *cls, ovs_version_t version,
                  struct flow *flow, struct flow_wildcards *wc)
{
    if (cls->backend == BACKEND_TSS) {
        return tss_lookup(cls->tss, version, flow, wc);
    } else if (cls->backend == BACKEND_DT) {
        return dt_lookup(cls->dt, version, flow, wc);  // ⭐ 传递 version
    }
}
```

**关键**: DT 必须理解并尊重 version 语义！

---

## 总结

### Version 的核心作用

| 作用 | 说明 |
|------|------|
| **并发控制** | 支持多读者无锁并发访问 |
| **版本隔离** | 不同版本的读者看到不同状态 |
| **原子更新** | 一组修改使用同一版本号，原子可见 |
| **一致性** | 读者看到一致的快照，不会看到中间状态 |
| **性能** | 读操作无锁，写操作不阻塞读 |

### 关键概念

```
1. ✅ 每个规则有 [add_version, remove_version) 区间
2. ✅ 读者使用特定版本号查找
3. ✅ 规则在版本区间内可见
4. ✅ 版本号单调递增
5. ✅ 删除是"软删除"（修改 remove_version）
6. ✅ 真正删除需要等待所有旧读者完成（RCU）
```

### 实现要点

```c
// 插入规则
versions = {add: current_version, remove: MAX}

// 删除规则
versions.remove = current_version

// 查找规则
if (add_version <= query_version < remove_version) {
    // 规则可见
}
```

### DT 集成建议

```
1. ✅ 复用 OVS 的 struct versions
2. ✅ 在 dt_lookup() 中传递 version
3. ✅ 在返回规则前检查可见性
4. ✅ 支持规则的版本化插入和删除
5. ✅ 与 TSS 保持一致的版本语义
```

---

**文档创建时间**: 2025-10-19  
**作者**: GitHub Copilot  
**版本**: 1.0  
**相关文档**:
- WC_LIFECYCLE_TRACE.md
- TSS_CLASSIFICATION_MECHANISM.md
- DT_INTEGRATION_DESIGN.md
