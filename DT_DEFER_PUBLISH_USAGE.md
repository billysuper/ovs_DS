# DT Defer/Publish 使用指南

## 概述

DT (Decision Tree) 分类器现在支持 defer/publish 机制，类似于 TSS (Tuple Space Search) 分类器的批量操作优化。这个机制允许你累积多个修改操作，然后一次性发布，从而提高性能并减少 RCU 同步开销。

## 设计原理

### TSS vs DT 的 Defer/Publish

| 特性 | TSS Classifier | DT Classifier |
|------|----------------|---------------|
| **数据结构** | pvector (优先级向量) | 决策树 (binary tree) |
| **Defer 机制** | 延迟 pvector_publish() | 在 temp_root 累积修改 |
| **Publish 机制** | pvector_publish() 更新向量 | 原子切换 root → temp_root |
| **并发保护** | RCU + pvector 内部 COW | RCU + 完整树 COW |
| **优化目标** | 减少 pvector 重建次数 | 减少树重建和 RCU 同步 |

### DT Defer/Publish 工作流程

```
正常模式 (publish=true):
┌──────────────┐
│ dt_insert()  │ ──→ 立即修改 root ──→ ovsrcu_set(&dt->root, new_tree)
└──────────────┘                      ovsrcu_postpone(free, old_tree)

延迟模式 (publish=false):
┌──────────────┐
│ dt_defer()   │ ──→ temp_root = root, publish = false
└──────────────┘

┌──────────────┐
│ dt_insert()  │ ──→ 修改 temp_root ──→ ovsrcu_set_hidden(&dt->temp_root, new_tree)
│ (multiple)   │     (不发布)           (不释放旧树)
└──────────────┘

┌──────────────┐
│ dt_publish() │ ──→ root = temp_root ──→ ovsrcu_set(&dt->root, temp_root)
└──────────────┘     publish = true       ovsrcu_postpone(free, old_root)
```

## 使用方法

### 基本用法

```c
#include "dt-classifier.h"

struct decision_tree dt;
dt_init(&dt);

// === 批量插入示例 ===

// 1. 进入延迟模式
dt_defer(&dt);

// 2. 执行多次插入（在 temp_root 中累积）
for (int i = 0; i < 1000; i++) {
    struct cls_rule *rule = create_rule(i);
    dt_insert_rule(&dt, rule, OVS_VERSION_MIN);
}

// 3. 一次性发布所有修改
dt_publish(&dt);
```

### 与 OpenFlow Bundle 集成

```c
// 在 ofproto 中处理 OpenFlow bundle
void handle_bundle_commit(struct ofproto *ofproto, struct ofputil_bundle_msg *msg)
{
    struct oftable *table = &ofproto->tables[msg->table_id];
    
    // 进入延迟模式
    dt_defer(&table->dt);
    
    // 处理 bundle 中的所有流表修改
    for (size_t i = 0; i < msg->n_msgs; i++) {
        struct ofputil_flow_mod *fm = &msg->msgs[i];
        
        switch (fm->command) {
        case OFPFC_ADD:
            dt_insert_rule(&table->dt, &fm->rule->cr, fm->version);
            break;
        case OFPFC_DELETE:
            dt_remove_rule(&table->dt, &fm->rule->cr);
            break;
        // ... 其他命令
        }
    }
    
    // 原子地提交所有修改
    dt_publish(&table->dt);
    
    VLOG_INFO("Bundle committed: %zu rules modified atomically", msg->n_msgs);
}
```

### 错误处理与回滚

```c
bool batch_insert_with_rollback(struct decision_tree *dt, 
                                 struct cls_rule **rules, size_t n_rules)
{
    // 保存当前状态
    struct dt_node *backup = ovsrcu_get_protected(struct dt_node *, &dt->root);
    
    // 进入延迟模式
    dt_defer(dt);
    
    // 尝试批量插入
    bool success = true;
    for (size_t i = 0; i < n_rules; i++) {
        if (!dt_insert_rule(dt, rules[i], OVS_VERSION_MIN)) {
            VLOG_ERR("Failed to insert rule %zu", i);
            success = false;
            break;
        }
    }
    
    if (success) {
        // 成功：提交所有修改
        dt_publish(dt);
        VLOG_INFO("Batch insert successful: %zu rules", n_rules);
    } else {
        // 失败：放弃修改，恢复原状态
        ovsrcu_set_hidden(&dt->temp_root, backup);
        dt->publish = true;  // 退出延迟模式但不发布
        VLOG_WARN("Batch insert failed, rolled back");
    }
    
    return success;
}
```

## 性能优势

### RCU 同步开销对比

```
场景：插入 1000 个规则

┌─────────────────────┬──────────────┬──────────────┬─────────┐
│ 操作模式            │ RCU 同步次数 │ 树重建次数   │ 相对性能│
├─────────────────────┼──────────────┼──────────────┼─────────┤
│ 立即模式 (无 defer) │ 1000 次      │ 1000 次      │ 1x      │
│ 延迟模式 (defer)    │ 1 次         │ 1000 次*     │ 5-10x   │
│ Lazy Build          │ 1 次         │ 1 次         │ 100x+   │
└─────────────────────┴──────────────┴──────────────┴─────────┘

* 树重建是 COW 的副作用，但只在延迟模式下累积，最后一次性同步
```

### 内存使用

```
立即模式:
  - 每次插入: 旧树 + 新树 (在 RCU 宽限期内)
  - 峰值内存: ~2x 树大小

延迟模式:
  - 批量插入: root (旧) + temp_root (多次 COW)
  - 发布时: root + temp_root (短暂)
  - 峰值内存: ~2x 树大小 (类似，但 RCU 宽限期更短)
  
优势: 延迟模式将 1000 次 RCU 宽限期合并为 1 次
```

## 最佳实践

### 1. 何时使用 Defer/Publish

**✅ 适用场景：**
- OpenFlow bundle 操作（多个流表修改）
- 批量规则更新（>10 个规则）
- 配置文件加载（一次性加载大量规则）
- 定期规则清理（批量删除过期规则）

**❌ 不适用场景：**
- 单个规则插入/删除
- 实时响应的控制平面操作
- 内存受限环境（defer 会临时增加内存使用）

### 2. 与 Lazy Build 配合使用

```c
// 最佳初始化流程
struct decision_tree dt;
dt_init(&dt);

// 阶段 1: 使用 lazy build 加载初始规则
for (int i = 0; i < initial_rules_count; i++) {
    dt_add_rule_lazy(&dt, rules[i]);  // 不构建树
}

// 阶段 2: 一次性构建树
dt_build_initial_tree(&dt);

// 阶段 3: 运行时使用 defer/publish 批量更新
dt_defer(&dt);
for (int i = 0; i < update_count; i++) {
    dt_insert_rule(&dt, new_rules[i], version);
}
dt_publish(&dt);
```

### 3. 线程安全

```c
// DT 的 defer/publish 需要外部同步（与 TSS 相同）
static struct ovs_mutex dt_mutex = OVS_MUTEX_INITIALIZER;

void thread_safe_batch_insert(struct decision_tree *dt,
                              struct cls_rule **rules, size_t n)
{
    ovs_mutex_lock(&dt_mutex);
    
    dt_defer(dt);
    for (size_t i = 0; i < n; i++) {
        dt_insert_rule(dt, rules[i], OVS_VERSION_MIN);
    }
    dt_publish(dt);
    
    ovs_mutex_unlock(&dt_mutex);
}

// 读者不需要锁（RCU 保护）
const struct cls_rule *
thread_safe_lookup(struct decision_tree *dt, const struct flow *flow)
{
    // 不需要加锁，RCU 保护
    return dt_lookup(dt, OVS_VERSION_MIN, flow, NULL);
}
```

## 实现细节

### 数据结构

```c
struct decision_tree {
    OVSRCU_TYPE(struct dt_node *) root;       // 已发布的树（读者看到）
    OVSRCU_TYPE(struct dt_node *) temp_root;  // 临时树（累积修改）
    bool publish;                              // true=立即模式, false=延迟模式
    // ...
};
```

### 关键函数

```c
// 进入延迟模式
void dt_defer(struct decision_tree *dt) {
    if (dt->publish) {
        dt->publish = false;
        dt->temp_root = dt->root;  // 复制当前根
    }
}

// 发布修改
void dt_publish(struct decision_tree *dt) {
    if (!dt->publish) {
        dt->publish = true;
        ovsrcu_set(&dt->root, dt->temp_root);      // 原子切换
        ovsrcu_postpone(dt_node_destroy, old_root); // 延迟释放
        dt->temp_root = NULL;
    }
}

// 获取工作根
static inline struct dt_node **
dt_get_working_root_ptr(struct decision_tree *dt) {
    return dt->publish ? &dt->root : &dt->temp_root;
}
```

## 调试与监控

### 日志输出

```c
// 启用 DT 调试日志
ovs-appctl vlog/set dt_classifier:dbg

// 示例输出
DT: Entered deferred mode
DT: Inserted rule (priority=100) using COW (deferred), total=1
DT: Inserted rule (priority=200) using COW (deferred), total=2
...
DT: Published changes (old_root=0x12345, new_root=0x67890)
```

### 性能计数器

```c
// 添加性能统计
struct dt_perf_stats {
    uint64_t n_defers;
    uint64_t n_publishes;
    uint64_t n_batched_inserts;
    uint64_t avg_batch_size;
};

// 在 dt_publish() 中更新
dt->stats.n_publishes++;
dt->stats.n_batched_inserts += batched_operations;
```

## 总结

DT 的 defer/publish 机制提供了与 TSS 类似的批量优化能力，但基于决策树的 COW 特性实现。主要优势是：

1. **减少 RCU 同步**: 将 N 次同步合并为 1 次
2. **原子性**: 批量修改要么全部可见，要么全部不可见
3. **兼容性**: 与现有 OpenFlow bundle 机制无缝集成
4. **可扩展**: 支持未来的事务性操作

结合 lazy build 和 defer/publish，DT 在初始化和批量更新场景下都能达到最佳性能。
