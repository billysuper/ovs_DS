# 为什么 DT 需要整棵树层面的 Defer/Publish？

## 🎯 核心问题

**为什么不能只在 leaf 层面做 defer/publish？为什么需要整棵树发布？**

---

## 📊 场景对比

### 场景 1：单个规则插入（简单情况）

```
插入前的树：
        Root (Internal)
       /              \
    Field A        Field B
    /    \         /    \
  L1    L2       L3    L4
  [R1]  [R2]    [R3]  [R4]

插入 R5 到 L1：
        Root (Internal)
       /              \
    Field A        Field B
    /    \         /    \
  L1'   L2       L3    L4
  [R1   [R2]    [R3]  [R4]
   R5]
```

**如果只在 leaf 发布：**
```c
// 只复制 L1 -> L1'
ovsrcu_set(&node_A->left, new_L1);  // 更新指针
```

**看起来可行！但是...**

---

### 场景 2：批量插入（问题来了！）

```
批量插入 100 个规则，分布在不同 leaf：

插入序列：
  R5  -> L1
  R6  -> L2
  R7  -> L1
  R8  -> L3
  R9  -> L2
  ...
  R104 -> L4
```

#### ❌ 方案 A：每个 leaf 独立发布

```c
void dt_insert_rule_leaf_only(struct decision_tree *dt, 
                              const struct cls_rule *rule)
{
    // 1. 找到目标 leaf
    struct dt_leaf_node *leaf = dt_find_leaf(dt->root, rule);
    
    // 2. COW leaf
    struct dt_leaf_node *new_leaf = dt_leaf_cow(leaf);
    dt_leaf_add_rule(new_leaf, rule);
    
    // 3. 立即发布 leaf（问题！）
    struct dt_internal_node *parent = dt_find_parent(dt->root, leaf);
    ovsrcu_set(&parent->left_or_right, new_leaf);  // ← 立即可见！
    ovsrcu_postpone(free, leaf);
}

// 批量插入 100 个规则
for (int i = 0; i < 100; i++) {
    dt_insert_rule_leaf_only(dt, rules[i]);  // ← 每次都立即可见
}
```

**问题 1：中间状态可见**
```
Reader 线程在插入过程中看到：
  T0: 看到 R5 (只有 1 个新规则)
  T1: 看到 R5, R6 (有 2 个新规则)
  T2: 看到 R5, R6, R7 (有 3 个新规则)
  ...
  T99: 看到所有 100 个规则

每个中间状态都会触发分类逻辑！
```

**问题 2：性能灾难**
```c
// 每次插入都触发 RCU 同步
for (int i = 0; i < 100; i++) {
    dt_insert_rule_leaf_only(dt, rules[i]);
    ovsrcu_postpone(free, old_leaf);  // ← 100 次 RCU 回调！
}

// 实际开销：
// - 100 次 ovsrcu_set() 的内存屏障
// - 100 次 ovsrcu_postpone() 的回调注册
// - 可能触发 100 次 ovsrcu_quiesce()
```

---

#### ✅ 方案 B：整棵树 defer/publish

```c
// 批量插入
dt_defer(dt);  // ← 进入 defer 模式

for (int i = 0; i < 100; i++) {
    dt_insert_rule(dt, rules[i]);  // ← 修改 temp_root
}

dt_publish(dt);  // ← 一次性发布所有修改
```

**工作原理：**
```
T0: root = 原始树
    temp_root = 原始树副本

T1: 插入 R5 -> temp_root 修改
    root 不变（reader 看不到）

T2: 插入 R6 -> temp_root 修改
    root 不变（reader 看不到）

...

T99: 插入 R104 -> temp_root 修改
     root 不变（reader 看不到）

T100: dt_publish() -> root = temp_root
      ↑ 一次性原子切换！
      Reader 立即看到所有 100 个规则！
```

**优势：**
```c
void dt_publish(struct decision_tree *dt)
{
    struct dt_node *old_root = ovsrcu_get(struct dt_node *, &dt->root);
    struct dt_node *new_root = ovsrcu_get(struct dt_node *, &dt->temp_root);
    
    // 1. 一次原子切换
    ovsrcu_set(&dt->root, new_root);  // ← 只有一次！
    
    // 2. 一次 RCU 回调
    ovsrcu_postpone(dt_destroy_subtree, old_root);  // ← 只有一次！
    
    dt->publish = true;
}

// 性能对比：
// 方案 A: 100 次 ovsrcu_set + 100 次 ovsrcu_postpone
// 方案 B: 1 次 ovsrcu_set + 1 次 ovsrcu_postpone
// 性能提升：100 倍！
```

---

## 🔍 为什么不能只复制受影响的 leaf？

### 误解：认为可以只更新部分树

```c
// 错误想法：只 COW 受影响的 path
void dt_insert_with_partial_cow(struct decision_tree *dt, 
                                const struct cls_rule *rule)
{
    // 1. 找到 path
    struct dt_path path = dt_find_path(dt->root, rule);
    
    // 2. 只 COW leaf
    struct dt_leaf_node *new_leaf = dt_leaf_cow(path.leaf);
    dt_leaf_add_rule(new_leaf, rule);
    
    // 3. 更新父节点指针（问题！）
    struct dt_internal_node *parent = path.nodes[path.depth - 1];
    ovsrcu_set(&parent->left, new_leaf);  // ← 破坏了 RCU 语义！
}
```

**为什么不行？RCU 的核心约束！**

```
原子性要求：
  Reader 要么看到：
    (a) 完全的旧状态
    (b) 完全的新状态
  
  不能看到：
    (c) 混合状态（部分旧 + 部分新）

如果只更新 parent->left：
  
  Reader 可能看到：
    Root (旧)
      -> Internal Node (旧)
          -> Left (新 leaf!)  ← 已更新
          -> Right (旧 leaf)  ← 未更新
  
  这是混合状态！违反 RCU 语义！
```

---

## 🎯 正确的 COW 路径重建

### 为什么需要重建整条路径？

```c
// 正确的 COW：从 leaf 到 root 的完整路径
struct dt_node *dt_insert_with_full_path_cow(struct decision_tree *dt,
                                             const struct cls_rule *rule)
{
    struct dt_path path = dt_find_path(dt->root, rule);
    
    // 1. COW leaf
    struct dt_leaf_node *new_leaf = dt_leaf_cow(path.leaf);
    dt_leaf_add_rule(new_leaf, rule);
    
    struct dt_node *current = (struct dt_node *)new_leaf;
    
    // 2. 从下往上重建路径
    for (int i = path.depth - 1; i >= 0; i--) {
        struct dt_internal_node *old_internal = path.nodes[i];
        
        // COW 内部节点
        struct dt_internal_node *new_internal = dt_internal_cow(old_internal);
        
        // 更新指针
        if (path.directions[i] == 0) {
            new_internal->left = current;  // 指向新子树
            // right 保持不变（共享旧子树）
        } else {
            new_internal->right = current;  // 指向新子树
            // left 保持不变（共享旧子树）
        }
        
        current = (struct dt_node *)new_internal;
    }
    
    // 3. 返回新 root
    return current;  // 完整的新树
}
```

**关键点：整条路径都是新的！**

```
旧树：
        Root_old
       /        \
    A_old      B_old
    /   \      /   \
  L1   L2    L3   L4

插入 R5 到 L1 后的新树：
        Root_new  ← 新的！
       /        \
    A_new      B_old  ← 共享旧子树
    /   \      /   \
  L1'  L2    L3   L4
  ↑    ↑
  新   共享旧节点

完整的 COW 路径：Root_new -> A_new -> L1'
共享的部分：B_old, L2, L3, L4
```

---

## 💡 为什么需要整棵树发布？

### 原因 1：**批量操作的原子性**

```c
// OpenFlow Bundle 示例
void ofproto_flow_mod_bundle(struct ofproto *ofproto,
                            struct flow_mod *mods, size_t n_mods)
{
    // 1. 开始事务
    classifier_defer(&ofproto->cls);
    
    // 2. 批量修改（所有修改在 temp_root）
    for (size_t i = 0; i < n_mods; i++) {
        classifier_insert(&ofproto->cls, mods[i].rule);
    }
    
    // 3. 提交事务（一次性发布）
    classifier_publish(&ofproto->cls);
    // ↑ 所有修改同时可见！
}
```

**如果只在 leaf 发布：**
- ❌ 每个规则插入都立即可见
- ❌ 无法保证批量操作的原子性
- ❌ OpenFlow Bundle 语义无法实现

---

### 原因 2：**性能优化**

```c
// 性能对比

// 方案 A：每次插入都发布（leaf 级别）
for (int i = 0; i < N; i++) {
    insert_and_publish_leaf(rules[i]);
    // 开销：N 次内存屏障 + N 次 RCU 回调
}
// 总开销：O(N) 次同步操作

// 方案 B：批量发布（tree 级别）
defer();
for (int i = 0; i < N; i++) {
    insert_to_temp_tree(rules[i]);
    // 开销：只是内存操作
}
publish();  // 开销：1 次内存屏障 + 1 次 RCU 回调
// 总开销：O(1) 次同步操作

// 性能提升：N 倍！
```

---

### 原因 3：**一致性保证**

```c
// 示例：插入相关的规则

// 规则集：
//   R1: priority=100, match=ip_src=10.0.0.1
//   R2: priority=90,  match=ip_src=10.0.0.0/24
//   R3: priority=80,  match=ip_src=0.0.0.0/0

// 期望：所有规则同时生效，优先级正确

// 如果只在 leaf 发布：
T0: 插入 R1 (立即可见)
    查询 10.0.0.1 -> 匹配 R1 ✓
    查询 10.0.0.2 -> 无匹配 ✗ (R2 还没生效)

T1: 插入 R2 (立即可见)
    查询 10.0.0.2 -> 匹配 R2 ✓
    查询 192.168.0.1 -> 无匹配 ✗ (R3 还没生效)

T2: 插入 R3 (立即可见)
    查询 192.168.0.1 -> 匹配 R3 ✓

// 问题：中间状态不一致！

// 如果整棵树发布：
defer();
  插入 R1, R2, R3 (都在 temp_root，不可见)
publish();
  // 所有规则同时可见，一致性保证！
```

---

### 原因 4：**RCU 内存回收效率**

```c
// 方案 A：每次插入都回收
for (int i = 0; i < 100; i++) {
    insert_rule(rules[i]);
    ovsrcu_postpone(free, old_leaf_i);  // ← 100 个回调
}

// RCU 需要等待：
// - 100 次 grace period (可能需要多个 quiesce 周期)
// - 100 个回调函数执行
// - 内存碎片化

// 方案 B：批量回收
defer();
for (int i = 0; i < 100; i++) {
    insert_rule(rules[i]);  // 修改 temp_root
}
publish();
ovsrcu_postpone(free_subtree, old_root);  // ← 1 个回调

// RCU 优化：
// - 1 次 grace period
// - 1 个回调函数（递归释放整棵树）
// - 内存一次性回收，减少碎片
```

---

## 📊 整棵树发布 vs Leaf 发布对比

| 维度 | Leaf 级别发布 | Tree 级别发布 |
|------|-------------|-------------|
| **原子性** | ❌ 中间状态可见 | ✅ 批量操作原子 |
| **性能** | ❌ N 次同步开销 | ✅ O(1) 同步开销 |
| **一致性** | ❌ 规则逐个生效 | ✅ 规则同时生效 |
| **RCU 效率** | ❌ N 次回调 | ✅ 1 次回调 |
| **复杂度** | ⚠️ 需要处理并发更新 | ✅ 简单：双缓冲 |
| **OpenFlow 兼容** | ❌ 无法支持 Bundle | ✅ 完美支持 |

---

## 🎯 最终结论

### ✅ 整棵树发布是必须的！

**核心原因：**

1. **RCU 语义要求**
   - Reader 要么看到完全的旧状态，要么看到完全的新状态
   - 不能有混合状态
   - 需要 COW 整条路径，最终需要切换 root

2. **批量操作性能**
   - 100 个规则插入：O(N) -> O(1) 同步开销
   - 减少内存屏障和 RCU 回调次数

3. **OpenFlow 规范要求**
   - Bundle 需要事务性：要么全成功，要么全失败
   - 所有修改必须同时可见

4. **内存回收效率**
   - 一次性回收整棵树，减少碎片
   - 减少 RCU grace period 等待

### 🔑 关键设计点

```c
struct decision_tree {
    OVSRCU_TYPE(struct dt_node *) root;       // 已发布的树
    OVSRCU_TYPE(struct dt_node *) temp_root;  // 工作树
    bool publish;                             // 是否立即发布
    int defer_depth;                          // 嵌套深度
};

// defer/publish 模式
dt_defer(dt);        // root 冻结，所有修改到 temp_root
  insert_rule(...);  // 在 temp_root 上 COW
  delete_rule(...);  // 在 temp_root 上 COW
dt_publish(dt);      // 原子切换：root = temp_root
```

### 💡 Leaf 发布的适用场景

**只有一种情况适合 leaf 级别发布：单次操作！**

```c
// 单次插入一个规则（立即发布模式）
if (dt->publish) {  // 没有 defer
    struct dt_node *new_root = dt_insert_with_cow(dt->root, rule);
    ovsrcu_set(&dt->root, new_root);  // 立即发布
}
```

**但这已经是 tree 级别的发布了（切换 root），而不是 leaf 级别！**

---

## 🎓 总结

**为什么需要整棵树发布？**

因为：
1. ✅ **RCU 要求原子可见性** - 需要切换 root
2. ✅ **批量操作需要事务性** - defer/publish 模式
3. ✅ **性能优化** - 减少同步开销
4. ✅ **一致性保证** - 规则集同时生效

**Leaf 层面不能独立发布的原因：**
1. ❌ 违反 RCU 原子性语义
2. ❌ 无法支持批量操作
3. ❌ 性能低下（N 次同步）
4. ❌ 不符合 OpenFlow 规范

**DT 的设计是正确的：defer/publish 在 tree 层面！**
