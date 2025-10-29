# TSS Defer/Publish 架构深度解析

## 🎯 重要澄清：TSS 的 Defer/Publish 在哪一层？

### ❌ 常见误解

**误解：TSS 在每个 subtable 上做 defer/publish**

```c
// 错误理解
struct cls_subtable {
    struct cmap rules;     // ← 以为 defer/publish 在这里
    bool publish;          // ✗ subtable 没有这个字段！
};
```

---

### ✅ 正确理解

**TSS 的 defer/publish 在 Classifier 层面，作用于 pvector！**

```c
/* lib/classifier.h */
struct classifier {
    int n_rules;
    struct cmap subtables_map;      // subtable 的 hash map
    struct pvector subtables;       // ← defer/publish 作用在这里！
    bool publish;                   // ← 控制标志在 classifier 层面
    // ...
};
```

---

## 📊 TSS 的层次结构

```
Classifier (最外层)
 │
 ├─ publish (bool)              ← defer/publish 控制在这里
 │
 ├─ pvector subtables           ← defer/publish 作用在这里
 │   ├─ Subtable[0] (优先级最高)
 │   ├─ Subtable[1]
 │   ├─ Subtable[2]
 │   └─ ...
 │
 └─ cmap subtables_map
     └─ Subtable
         ├─ cmap rules          ← cmap 内部有自己的 COW（不需要外部 defer）
         ├─ rculist rules_list  ← 原子操作（不需要 defer）
         └─ ccmap indices       ← ccmap 也有内部 COW
```

---

## 🔍 为什么在 pvector 层面？

### pvector 的特性

```c
/* lib/pvector.h */
struct pvector {
    size_t size;       /* Number of entries in the vector. */
    size_t allocated;  /* Number of allocated entries. */
    struct pvec_entry {
        int priority;
        void *ptr;
    } *vector;
    
    OVSRCU_TYPE(struct pvector_impl *) impl;  // ← 双缓冲
    struct pvector_impl *temp;                // ← 工作缓冲
};

/* 发布机制 */
void pvector_publish__(struct pvector *pvec)
{
    // 1. 排序 temp
    qsort(pvec->temp->vector, pvec->temp->size, 
          sizeof *pvec->temp->vector, pvector_entry_cmp);
    
    // 2. 原子切换
    ovsrcu_set(&pvec->impl, pvec->temp);
    
    // 3. 延迟释放旧 impl
    ovsrcu_postpone(free, old_impl);
}
```

### 关键操作：插入 subtable

```c
/* lib/classifier.c: classifier_replace() */

// 插入新规则到 subtable 后
if (n_rules == 1) {
    // 新 subtable，插入到 pvector
    subtable->max_priority = rule->priority;
    subtable->max_count = 1;
    pvector_insert(&cls->subtables, subtable, rule->priority);
    // ↑ 修改 pvector，需要重新排序
    
} else if (rule->priority > subtable->max_priority) {
    // 规则优先级变化，更新 pvector
    subtable->max_priority = rule->priority;
    subtable->max_count = 1;
    pvector_change_priority(&cls->subtables, subtable, rule->priority);
    // ↑ 修改 pvector，需要重新排序
}

// 检查是否立即发布
if (cls->publish) {
    pvector_publish(&cls->subtables);  // ← 重新排序！O(M log M)
}
```

---

## 💡 为什么 pvector 需要 defer/publish？

### 性能开销分析

```c
// 场景：批量插入 100 个规则，分布在 10 个不同 subtable

// === 没有 defer/publish ===
for (int i = 0; i < 100; i++) {
    classifier_insert(cls, rules[i]);
    pvector_publish(&cls->subtables);  // ← 可能触发 100 次排序！
    // 每次排序：O(M log M)，M = subtable 数量（假设 M=50）
}
// 总开销：100 * O(50 log 50) ≈ 100 * 282 = 28,200 次比较

// === 使用 defer/publish ===
cls->publish = false;  // defer
for (int i = 0; i < 100; i++) {
    classifier_insert(cls, rules[i]);
    // 不发布，只修改 temp
}
cls->publish = true;
pvector_publish(&cls->subtables);  // ← 只排序 1 次！
// 总开销：1 * O(50 log 50) ≈ 282 次比较

// 性能提升：28,200 / 282 ≈ 100 倍！
```

---

## 🔑 TSS vs DT 的 Defer/Publish 对比

### TSS 架构

```
Classifier                     ← defer/publish 控制层
  ├─ bool publish
  └─ pvector subtables         ← defer/publish 作用层（排序开销大）
      └─ Subtable
          ├─ cmap rules        ← 内部自动 COW
          └─ rculist rules_list ← 原子操作
```

**TSS 特点：**
- ✅ defer/publish 在 **Classifier** 层面
- ✅ 作用于 **pvector**（需要排序）
- ✅ subtable 内部的 cmap/rculist **不需要** defer/publish

---

### DT 架构

```
Decision Tree                  ← defer/publish 控制层
  ├─ bool publish
  ├─ root                      ← 已发布的树
  └─ temp_root                 ← defer/publish 作用层（树重建）
      └─ Tree Structure
          ├─ Internal Node     ← COW 路径重建
          └─ Leaf Node         ← 简单数组
              └─ rules[]
```

**DT 特点：**
- ✅ defer/publish 在 **Decision Tree** 层面
- ✅ 作用于 **整棵树**（避免频繁 COW 路径重建）
- ✅ leaf 内部的数组 **不需要** defer/publish

---

## 📊 关键数据结构的 COW 机制

### 1. cmap（无需外部 defer/publish）

```c
struct cmap {
    OVSRCU_TYPE(struct cmap_impl *) impl;  // ← 内部 COW
};

// 插入时自动 COW
size_t cmap_insert(struct cmap *cmap, ...)
{
    if (impl->n >= impl->max_n) {
        impl = cmap_rehash(cmap, ...);  // ← 自动创建新 impl，COW！
    }
    // ...
}
```

**cmap 不需要外部 defer/publish，因为它内部已经实现了 COW！**

---

### 2. pvector（需要外部 defer/publish）

```c
struct pvector {
    OVSRCU_TYPE(struct pvector_impl *) impl;  // 已发布
    struct pvector_impl *temp;                // 工作缓冲
};

// 插入需要手动 publish
void pvector_insert(struct pvector *pvec, void *ptr, int priority)
{
    // 修改 temp（不可见）
    pvec->temp->vector[pvec->temp->size++] = (struct pvec_entry){
        .priority = priority,
        .ptr = ptr
    };
    // 不自动排序！需要显式调用 pvector_publish()
}

void pvector_publish__(struct pvector *pvec)
{
    qsort(pvec->temp->vector, ...);  // ← 手动排序
    ovsrcu_set(&pvec->impl, pvec->temp);
}
```

**pvector 需要外部 defer/publish，因为排序开销大！**

---

### 3. rculist（无需 defer/publish）

```c
// rculist 使用原子操作
void rculist_push_back(struct rculist *list, struct rculist_node *node)
{
    struct rculist_node *tail = rculist_back_protected(list);
    
    node->prev = tail;
    node->next = &list->sentinel;
    
    // 原子操作
    ovsrcu_set(&tail->next, node);
    list->sentinel.prev = node;
}
```

**rculist 无需 defer/publish，因为操作已经是原子的！**

---

## 🎯 总结对比表

| 数据结构 | 在 TSS 中的位置 | 需要 defer/publish？ | 原因 |
|---------|---------------|-------------------|------|
| **pvector** | Classifier 层 | ✅ **需要** | 排序开销大 O(M log M) |
| **cmap** | Subtable 内部 | ❌ 不需要 | 内部自动 COW |
| **rculist** | Subtable 内部 | ❌ 不需要 | 原子操作 |
| **ccmap** | Subtable 内部 | ❌ 不需要 | 内部自动 COW |
| **整棵树** | DT 层 | ✅ **需要** | COW 路径重建开销大 |

---

## 💡 关键洞察

### 1. TSS 的 defer/publish **不是**在 subtable 层面

```c
// ✗ 错误理解
struct cls_subtable {
    bool publish;  // subtable 没有这个！
};

// ✓ 正确理解
struct classifier {
    bool publish;  // ← defer/publish 在 classifier 层面
    struct pvector subtables;  // ← 作用于 pvector
};
```

---

### 2. subtable 内部的数据结构已经是并发安全的

```c
struct cls_subtable {
    struct cmap rules;          // ← 内部 COW（自动）
    struct rculist rules_list;  // ← 原子操作（自动）
    struct ccmap indices[...];  // ← 内部 COW（自动）
};

// 插入规则到 subtable（无需 defer/publish）
void subtable_insert_rule(struct cls_subtable *subtable, ...)
{
    cmap_insert(&subtable->rules, ...);  // ← 自动 COW
    rculist_push_back(&subtable->rules_list, ...);  // ← 原子操作
    // 这些操作本身就是并发安全的！
}
```

---

### 3. defer/publish 只作用于**需要批量优化**的层次

| 系统 | defer/publish 层次 | 批量优化的目标 |
|------|------------------|--------------|
| **TSS** | Classifier 层 | 避免频繁排序 pvector |
| **DT** | Decision Tree 层 | 避免频繁 COW 路径重建 |

---

## 🎓 最终答案

### ❌ TSS **不是**在 subtable 上做 defer/publish

### ✅ TSS 在 **Classifier 层面**做 defer/publish

**作用对象：pvector（subtable 容器）**

**原因：避免频繁排序**

**证据：**
```c
// lib/classifier.h
struct classifier {
    bool publish;              // ← 控制在这里
    struct pvector subtables;  // ← 作用在这里
};

// lib/classifier.c
if (cls->publish) {
    pvector_publish(&cls->subtables);  // ← 发布在这里
}
```

---

## 🔄 类比理解

```
TSS:
  [Classifier]
    └─ defer/publish 控制
        └─ [pvector] ← 需要排序，所以需要 defer/publish
            └─ [subtable] ← 内部数据结构已经并发安全

DT:
  [Decision Tree]
    └─ defer/publish 控制
        └─ [整棵树] ← 需要 COW 路径重建，所以需要 defer/publish
            └─ [leaf] ← 内部数组不需要额外 defer/publish
```

**都是在最外层控制，作用于需要批量优化的数据结构！**
