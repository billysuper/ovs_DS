# OVS 规则替换机制深度解析

## 🎯 核心问题

**OVS 不会对分类器做规则取代吗？为什么不需要 replace API？**

---

## 🔍 答案：会！但替换逻辑在 **ofproto 层面**实现

---

## 📊 OVS 的规则更新流程

### 场景：用户修改一个流表规则

```bash
# OpenFlow 命令
ovs-ofctl mod-flows br0 "priority=100,ip,nw_src=10.0.0.1,actions=drop"
```

---

### 第 1 步：查找是否存在相同规则

```c
// ofproto/ofproto.c: add_flow_start() - 行 5310

/* 检查是否存在完全相同的规则 */
old_rule = rule_from_cls_rule(
    classifier_find_rule_exactly(&table->cls, &new_rule->cr, ofm->version)
);
//  ↑ 使用 find_rule_exactly，不是 replace！

if (!old_rule) {
    // 新规则，直接插入
} else {
    // 找到旧规则，需要替换
    ofm->modify_cookie = true;
}
```

**关键：OVS 不直接调用 `classifier_replace()`，而是先用 `find_rule_exactly()` 查找！**

---

### 第 2 步：OVS 层面的替换逻辑

```c
// ofproto/ofproto.c: replace_rule_start() - 行 5734

static void
replace_rule_start(struct ofproto *ofproto, struct ofproto_flow_mod *ofm,
                   struct rule *old_rule, struct rule *new_rule)
{
    if (old_rule) {
        // 1. 复制旧规则的一些属性到新规则
        if (ofm->command != OFPFC_ADD) {
            new_rule->idle_timeout = old_rule->idle_timeout;
            new_rule->hard_timeout = old_rule->hard_timeout;
            new_rule->flags = old_rule->flags;
            // ...
        }
        
        // 2. 标记旧规则在下一个版本中不可见
        cls_rule_make_invisible_in_version(&old_rule->cr, ofm->version);
        
        // 3. 从 ofproto 数据结构中移除旧规则
        ofproto_rule_remove__(ofproto, old_rule);
    }
    
    // 4. 插入新规则到 ofproto
    ofproto_rule_insert__(ofproto, new_rule);
    
    // 5. 插入新规则到 classifier
    classifier_insert(&table->cls, &new_rule->cr, ofm->version, 
                     ofm->conjs, ofm->n_conjs);
    //  ↑ 调用的是 classifier_insert，不是 classifier_replace！
}
```

**关键发现：**
1. ✅ OVS 确实会替换规则
2. ✅ 但替换逻辑在 **ofproto 层面**
3. ✅ 对 classifier 来说，只是：删除旧规则 + 插入新规则

---

## 🔍 `classifier_replace()` 的真正作用

### TSS 内部实现

```c
// lib/classifier.c: classifier_insert() - 行 687

void classifier_insert(struct classifier *cls, const struct cls_rule *rule,
                      ovs_version_t version, ...)
{
    const struct cls_rule *displaced_rule = 
        classifier_replace(cls, rule, version, conj, n_conj);
    ovs_assert(!displaced_rule);  // ← 期望没有重复规则！
    //          ↑ 如果返回非 NULL，说明有重复规则，触发断言失败
}
```

**`classifier_insert()` 内部调用 `classifier_replace()`，但期望返回 NULL！**

---

### `classifier_replace()` 做什么？

```c
// lib/classifier.c: classifier_replace() - 行 514

const struct cls_rule *
classifier_replace(struct classifier *cls, const struct cls_rule *rule, ...)
{
    // ... 查找 subtable ...
    
    head = find_equal(subtable, rule->match.flow, hash);
    
    if (!head) {
        // 没有相同 match 的规则，直接插入
        // ... 插入逻辑 ...
        return NULL;
    } else {
        // 找到相同 match 的规则（可能有多个，按优先级排序）
        
        FOR_EACH_RULE_IN_LIST_PROTECTED (iter, prev, head) {
            if (rule->priority > iter->priority) {
                break;
            }
        }
        
        if (iter && rule->priority == iter->priority) {
            // 找到相同 priority 的规则 → 替换！
            cls_match_replace(prev, iter, new);
            old = iter->cls_rule;
            
            // ... 清理旧规则 ...
            
            return old;  // ← 返回被替换的旧规则
        } else {
            // 不同 priority，插入新规则
            cls_match_insert(prev, iter, new);
            return NULL;  // ← 没有替换
        }
    }
}
```

**`classifier_replace()` 的真正作用：**

1. **处理 subtable 内部的规则链表**
   - 同一个 match 模式可以有多个规则（不同 priority）
   - 形成一个链表：`rule1(pri=100) -> rule2(pri=90) -> rule3(pri=80)`

2. **检测并替换相同 priority 的规则**
   - 如果插入 `rule_new(pri=90)`，找到 `rule2(pri=90)`
   - 替换：`rule1(pri=100) -> rule_new(pri=90) -> rule3(pri=80)`
   - 返回旧的 `rule2`

3. **`classifier_insert()` 期望不会替换**
   - 如果返回非 NULL，触发 `ovs_assert(!displaced_rule)` 失败
   - 说明 OVS 应该在调用 `classifier_insert()` 前先删除旧规则

---

## 💡 为什么 OVS 不直接用 `classifier_replace()`？

### 原因 1：需要控制版本可见性

```c
// OVS 的替换流程
replace_rule_start(ofproto, ofm, old_rule, new_rule) {
    // 1. 旧规则在 version N 中不可见
    cls_rule_make_invisible_in_version(&old_rule->cr, version_N);
    
    // 2. 移除旧规则（但保留对象，用于 revert）
    ofproto_rule_remove__(ofproto, old_rule);
    
    // 3. 插入新规则，在 version N 中可见
    classifier_insert(&table->cls, &new_rule->cr, version_N, ...);
}

// 如果事务失败，可以回滚
replace_rule_revert(ofproto, old_rule, new_rule) {
    // 恢复旧规则
    ofproto_rule_insert__(ofproto, old_rule);
    cls_rule_restore_visibility(&old_rule->cr);
    
    // 删除新规则
    classifier_remove_assert(&table->cls, &new_rule->cr);
}
```

**版本控制需要：**
- 旧规则在新版本中不可见
- 新规则在新版本中可见
- 可以回滚（恢复旧规则）

**如果直接用 `classifier_replace()`：**
- ❌ 旧规则立即被删除，无法回滚
- ❌ 无法控制版本可见性

---

### 原因 2：需要传递额外信息

```c
// ofproto/ofproto.c: replace_rule_start()

if (old_rule) {
    // 复制旧规则的属性到新规则
    new_rule->idle_timeout = old_rule->idle_timeout;
    new_rule->hard_timeout = old_rule->hard_timeout;
    new_rule->created = old_rule->created;  // ← 保留创建时间
    
    if (!change_cookie) {
        new_rule->flow_cookie = old_rule->flow_cookie;  // ← 保留 cookie
    }
}
```

**规则修改时需要保留某些属性，但 classifier 不关心这些！**

---

### 原因 3：需要处理硬件卸载

```c
// ofproto/ofproto.c: replace_rule_finish()

// 调用硬件层的 rule_insert，传递被替换的规则
error = ofproto->ofproto_class->rule_insert(
    new_rule, 
    replaced_rule,      // ← 旧规则
    modify_keep_counts  // ← 是否保留计数器
);
```

**硬件层需要知道旧规则，以便：**
- 保留数据包/字节计数器
- 平滑切换硬件流表

---

## 🎯 对 DT 的影响

### ✅ DT 需要实现替换逻辑

**但可以有两种方式：**

---

### 方案 A：在 `dt_insert_rule()` 内部处理（推荐）

```c
bool dt_insert_rule(struct decision_tree *dt,
                   const struct cls_rule *rule,
                   ovs_version_t version)
{
    // 1. 找到目标 leaf
    struct dt_leaf_node *leaf = dt_find_leaf_for_rule(dt, rule);
    
    // 2. 检查 leaf 中是否有相同规则
    for (size_t i = 0; i < leaf->n_rules; i++) {
        if (cls_rule_equal(leaf->rules[i], rule)) {
            // 找到相同规则
            
            // 检查 priority 是否相同
            if (leaf->rules[i]->priority == rule->priority) {
                // 相同 priority → 替换（但这不应该发生！）
                // OVS 应该先调用 find_rule_exactly 检查
                VLOG_WARN("DT: Replacing rule with same priority");
                
                // 在 COW 后的新 leaf 中替换
                new_leaf->rules[i] = rule;
                return true;
            } else {
                // 不同 priority → 插入（不替换）
                // 继续正常插入流程
                break;
            }
        }
    }
    
    // 3. 没有找到相同规则，执行正常插入
    // ... COW 插入逻辑 ...
}
```

**优点：**
- ✅ API 简单，兼容 OVS
- ✅ 不需要额外的 replace API

**缺点：**
- ⚠️ 由于 OVS 会先 find_rule_exactly，replace 路径可能永远不会执行

---

### 方案 B：依赖 OVS 先删除（更简单）

```c
bool dt_insert_rule(struct decision_tree *dt,
                   const struct cls_rule *rule,
                   ovs_version_t version)
{
    // 假设 OVS 已经通过 find_rule_exactly 检查
    // 如果有旧规则，OVS 已经先删除
    
    // 直接执行插入，不检查重复
    // ... COW 插入逻辑 ...
    
    // 如果真的有重复（OVS bug），触发断言
    ovs_assert(!dt_has_duplicate(dt, rule));
}
```

**优点：**
- ✅ 最简单
- ✅ 符合 OVS 的使用模式

**缺点：**
- ⚠️ 依赖 OVS 正确调用
- ⚠️ 如果 OVS 有 bug，可能插入重复规则

---

## 📊 总结

### ❌ 误解：OVS 不需要规则替换

**真相：OVS 需要替换，但在 ofproto 层面实现！**

```
OVS 替换流程：
  1. find_rule_exactly() ← 查找旧规则
  2. remove() ← 删除旧规则
  3. insert() ← 插入新规则
  
分类器看到的：
  - 删除操作
  - 插入操作
  
分类器不需要：
  - 单独的 replace API
```

---

### ✅ DT 需要什么？

1. **✅ `dt_find_rule_exactly()`** - 必需
   - OVS 用它查找是否有旧规则
   
2. **✅ `dt_remove_rule()`** - 必需
   - OVS 用它删除旧规则
   
3. **✅ `dt_insert_rule()`** - 已实现
   - OVS 用它插入新规则

4. **❌ `dt_replace_rule()`** - 不需要
   - OVS 不会直接调用 replace

---

### 🔑 关键洞察

**`classifier_replace()` 是 TSS 的内部实现细节：**

- 用于处理 subtable 内部的规则链表
- 处理相同 match、相同 priority 的规则冲突
- `classifier_insert()` 内部调用，期望返回 NULL

**OVS 依赖的是 ofproto 层面的替换逻辑：**

- find → remove → insert
- 控制版本可见性
- 传递额外信息给硬件层

**DT 不需要实现 `dt_replace_rule()` API：**

- ✅ `dt_find_rule_exactly()` - 让 OVS 找到旧规则
- ✅ `dt_remove_rule()` - 让 OVS 删除旧规则
- ✅ `dt_insert_rule()` - 让 OVS 插入新规则

**完美！**
