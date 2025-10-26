# Decision Tree 增量建構機制

## 目錄
1. [從空樹開始的建構過程](#從空樹開始的建構過程)
2. [詳細案例演示](#詳細案例演示)
3. [樹分裂觸發機制](#樹分裂觸發機制)
4. [完整增量建構示例](#完整增量建構示例)
5. [性能分析](#性能分析)
6. [與批量建樹的對比](#與批量建樹的對比)

---

## 從空樹開始的建構過程

### 🌱 階段 0：空樹狀態

```c
struct decision_tree dt;
dt_init(&dt);

// 此時：
dt.root = NULL;
dt.n_rules = 0;
dt.n_leaf_nodes = 0;
dt.n_internal_nodes = 0;
```

**樹結構**：
```
(空)
```

---

### 📝 階段 1：插入第一條規則

**操作**：
```c
struct cls_rule *rule1 = create_rule(
    "priority=100, ip_src=192.168.1.0/24, action=allow"
);
dt_insert_rule(&dt, rule1, 0);
```

**代碼路徑**（`lib/dt-classifier.c:432-443`）：
```c
/* Empty tree case */
if (!old_root) {
    struct dt_node *new_root = dt_node_create_leaf();  // 創建葉節點
    rculist_push_back(&new_root->leaf.rules, &rule->node);
    new_root->leaf.n_rules = 1;
    
    ovsrcu_set(&dt->root, new_root);  // 設置為根
    dt->n_rules++;
    dt->n_leaf_nodes++;
    return true;
}
```

**樹結構**：
```
[Leaf]
 └─ Rule1 (priority=100)
```

**狀態**：
- `dt.root` → Leaf 節點
- `dt.n_rules = 1`
- `dt.n_leaf_nodes = 1`
- `dt.n_internal_nodes = 0`

---

### 📝 階段 2：插入第二條規則

**操作**：
```c
struct cls_rule *rule2 = create_rule(
    "priority=90, ip_src=10.0.0.0/8, action=deny"
);
dt_insert_rule(&dt, rule2, 0);
```

**代碼路徑**（`lib/dt-classifier.c:445-502`）：
```c
// 1. 樹不為空，獲取舊根
struct dt_node *old_root = ovsrcu_get(&dt->root);

// 2. 初始化路徑記錄
struct dt_path path;
dt_path_init(&path);

// 3. 找到插入點（目前是根節點，就是葉節點）
struct dt_node *node = old_root;
dt_path_record(&path, node, false);

// 4. 由於 node->type == DT_NODE_LEAF，跳過 while 循環

// 5. 複製葉節點
struct dt_node *new_leaf = dt_node_copy(node);

// 6. 按優先級插入規則
// Rule1: priority=100
// Rule2: priority=90  → 插入到 Rule1 後面
rculist_push_back(&new_leaf->leaf.rules, &rule2->node);
new_leaf->leaf.n_rules = 2;

// 7. 重建路徑（此時 path.depth=1，直接返回 new_leaf）
struct dt_node *new_root = dt_path_rebuild_cow(&path, new_leaf);

// 8. 原子性切換根
ovsrcu_set(&dt->root, new_root);

// 9. 延遲釋放舊節點
ovsrcu_postpone(dt_node_destroy, old_root);
```

**樹結構**：
```
[Leaf]
 ├─ Rule1 (priority=100)
 └─ Rule2 (priority=90)
```

**狀態**：
- `dt.root` → 新 Leaf 節點（COW 複製）
- `dt.n_rules = 2`
- `dt.n_leaf_nodes = 1`（仍然只有一個葉節點）

---

### 📝 階段 3：插入第三、四、五條規則

繼續插入規則，葉節點持續增長：

```c
rule3: priority=80, ip_dst=8.8.8.8/32
rule4: priority=70, tcp_dst=80
rule5: priority=60, tcp_dst=443
```

**樹結構**：
```
[Leaf]
 ├─ Rule1 (priority=100)
 ├─ Rule2 (priority=90)
 ├─ Rule3 (priority=80)
 ├─ Rule4 (priority=70)
 └─ Rule5 (priority=60)
```

**狀態**：
- `dt.n_rules = 5`
- `dt.n_leaf_nodes = 1`
- 每次插入都是 **COW** 複製葉節點
- 所有舊版本安全回收（RCU）

---

## ⚠️ 當前實現的問題

### 問題：**樹永遠不會分裂！**

**原因**：
1. **沒有自動分裂邏輯**
   - `dt_insert_rule()` 只負責插入規則到葉節點
   - 沒有檢查葉節點大小 (`max_leaf_size`)
   - 沒有觸發分裂操作

2. **缺少樹重構觸發器**
   - 批量建樹函數 `dt_build_tree()` 存在但從未被調用
   - 增量插入不會自動調用樹重構

**結果**：
```
即使插入 10,000 條規則，樹仍然是：

[Leaf with 10,000 rules]
 ├─ Rule1
 ├─ Rule2
 ├─ ...
 └─ Rule10000

查找性能：O(n) = O(10,000) ❌
應該是：O(log n) = O(13) ✅
```

---

## 🔧 解決方案：增量樹分裂機制

### 方案 A：插入時檢查分裂（推薦）

修改 `dt_insert_rule()` 在插入後檢查是否需要分裂：

```c
bool
dt_insert_rule(struct decision_tree *dt, const struct cls_rule *rule,
               ovs_version_t version)
{
    // ... 現有插入邏輯 ...
    
    /* NEW: Check if leaf node is too large */
    if (new_leaf->leaf.n_rules > dt->max_leaf_size) {
        /* Trigger split */
        struct dt_node *split_root = dt_split_leaf(dt, new_leaf, &path);
        new_root = dt_path_rebuild_cow(&path, split_root);
    }
    
    ovsrcu_set(&dt->root, new_root);
    ovsrcu_postpone(dt_node_destroy, old_root);
    
    return true;
}
```

### 方案 B：定期重建樹

```c
/* 計數器追蹤插入次數 */
static size_t insert_count = 0;

bool
dt_insert_rule(struct decision_tree *dt, const struct cls_rule *rule,
               ovs_version_t version)
{
    // ... 現有插入邏輯 ...
    
    insert_count++;
    
    /* 每 N 次插入後重建樹 */
    if (insert_count >= REBUILD_THRESHOLD) {
        dt_rebuild_tree(dt);
        insert_count = 0;
    }
    
    return true;
}
```

---

## 完整增量建構示例（理想實現）

假設 `max_leaf_size = 3`（實際應該更大，如 16）

### 插入規則 1-3：單葉節點

```
Rule1: ip_src=192.168.1.0/24
Rule2: ip_src=10.0.0.0/8  
Rule3: ip_src=172.16.0.0/12

樹結構：
[Leaf]
 ├─ Rule1
 ├─ Rule2
 └─ Rule3
```

### 插入規則 4：觸發分裂

```c
Rule4: ip_src=8.8.8.0/24

// 插入後 n_rules = 4 > max_leaf_size = 3
// 觸發分裂！
```

**分裂過程**：

1. **選擇分裂欄位**（`dt_select_split_field`）
   ```
   候選欄位：ip_src, ip_dst, tcp_src, ...
   統計使用次數：
     ip_src: 4 次 ✅（所有規則都用）
     ip_dst: 0 次
     tcp_src: 0 次
   
   選擇：ip_src
   ```

2. **選擇分裂值**（`dt_find_split_value`）
   ```
   收集所有 ip_src 值：
     192.168.1.0
     10.0.0.0
     172.16.0.0
     8.8.8.0
   
   排序：
     8.8.8.0
     10.0.0.0
     172.16.0.0
     192.168.1.0
   
   選擇中位數：(10.0.0.0 + 172.16.0.0) / 2 ≈ 91.8.0.0
   
   測試類型：PREFIX（因為是 IP 地址）
   分裂值：91.8.0.0/8
   ```

3. **分配規則到子節點**
   ```
   測試：ip_src 是否匹配 91.8.0.0/8?
   
   Left (不匹配):
     8.8.8.0/24 ❌
     10.0.0.0/8 ❌
   
   Right (匹配):
     172.16.0.0/12 ✅
     192.168.1.0/24 ✅
   ```

4. **創建新樹結構**
   ```
                [Internal]
              (ip_src PREFIX 91.8.0.0/8)
              /                    \
         [Leaf]                   [Leaf]
        ├─ Rule4                 ├─ Rule3
        └─ Rule2                 └─ Rule1
   ```

### 插入規則 5-7：填充葉節點

```
Rule5: ip_src=20.0.0.0/8  → Left leaf
Rule6: ip_src=200.0.0.0/8 → Right leaf
Rule7: ip_src=150.0.0.0/8 → Right leaf

樹結構：
                [Internal]
              (ip_src PREFIX 91.8.0.0/8)
              /                    \
         [Leaf: 3]               [Leaf: 4]
        ├─ Rule4                 ├─ Rule3
        ├─ Rule2                 ├─ Rule1
        └─ Rule5                 ├─ Rule6
                                 └─ Rule7
```

### 插入規則 8：觸發右側分裂

```
Rule8: ip_src=180.0.0.0/8 → Right leaf (4+1=5 > 3)

右側葉節點需要分裂！

分裂右側葉節點：
  收集值：172.16.0.0, 192.168.1.0, 200.0.0.0, 150.0.0.0, 180.0.0.0
  排序：150, 172, 180, 192, 200
  中位數：180.0.0.0
  
新樹結構：
                    [Internal]
                  (ip_src PREFIX 91.8.0.0/8)
                  /                    \
             [Leaf: 3]             [Internal]
            ├─ Rule4              (ip_src PREFIX 180.0.0.0/8)
            ├─ Rule2              /                    \
            └─ Rule5         [Leaf: 2]              [Leaf: 3]
                            ├─ Rule7                ├─ Rule8
                            └─ Rule3                ├─ Rule1
                                                    └─ Rule6
```

---

## 樹分裂觸發機制設計

### 觸發條件

```c
#define DEFAULT_MAX_LEAF_SIZE 16

struct decision_tree {
    // ... existing fields ...
    size_t max_leaf_size;      /* 葉節點最大規則數 */
    size_t split_threshold;    /* 分裂觸發閾值 */
};

/* 初始化時設置 */
void dt_init(struct decision_tree *dt) {
    // ... existing code ...
    dt->max_leaf_size = DEFAULT_MAX_LEAF_SIZE;
    dt->split_threshold = dt->max_leaf_size * 1.5;  // 留一些緩衝
}
```

### 分裂算法

```c
/* 分裂單個葉節點 */
static struct dt_node *
dt_split_leaf(struct decision_tree *dt, struct dt_node *leaf,
              struct dt_path *path)
{
    ovs_assert(leaf->type == DT_NODE_LEAF);
    
    if (leaf->leaf.n_rules <= dt->max_leaf_size) {
        return leaf;  /* 不需要分裂 */
    }
    
    /* 1. 選擇分裂欄位 */
    const struct mf_field *field = dt_select_split_field(
        &leaf->leaf.rules, leaf->leaf.n_rules
    );
    
    if (!field) {
        return leaf;  /* 無法找到好的分裂欄位 */
    }
    
    /* 2. 選擇分裂值 */
    enum dt_test_type test_type;
    ovs_be32 split_value;
    unsigned int plen;
    
    if (!dt_find_split_value(field, &leaf->leaf.rules, 
                             &test_type, &split_value, &plen)) {
        return leaf;
    }
    
    /* 3. 創建內部節點 */
    struct dt_node *internal = dt_node_create_internal();
    internal->internal.field = field;
    internal->internal.test_type = test_type;
    
    if (test_type == DT_TEST_EXACT) {
        internal->internal.test.exact.value = split_value;
    } else if (test_type == DT_TEST_PREFIX) {
        internal->internal.test.prefix.value = split_value;
        internal->internal.test.prefix.plen = plen;
    }
    
    /* 4. 創建左右葉節點 */
    struct dt_node *left_leaf = dt_node_create_leaf();
    struct dt_node *right_leaf = dt_node_create_leaf();
    
    /* 5. 分配規則 */
    const struct cls_rule *rule;
    RCULIST_FOR_EACH (rule, node, &leaf->leaf.rules) {
        union mf_value value;
        mf_get_value(field, &rule->match, &value);
        
        bool match = dt_evaluate_test(internal, &value);
        
        if (match) {
            rculist_push_back(&right_leaf->leaf.rules, &rule->node);
            right_leaf->leaf.n_rules++;
        } else {
            rculist_push_back(&left_leaf->leaf.rules, &rule->node);
            left_leaf->leaf.n_rules++;
        }
    }
    
    /* 6. 連接子節點 */
    ovsrcu_set_hidden(&internal->internal.left, left_leaf);
    ovsrcu_set_hidden(&internal->internal.right, right_leaf);
    
    /* 7. 更新統計 */
    dt->n_leaf_nodes++;  /* 2 新葉 - 1 舊葉 = +1 */
    dt->n_internal_nodes++;
    
    return internal;
}
```

### 遞歸分裂（深度優化）

```c
/* 遞歸分裂直到所有葉節點都符合大小限制 */
static struct dt_node *
dt_split_recursive(struct decision_tree *dt, struct dt_node *node)
{
    if (node->type == DT_NODE_INTERNAL) {
        /* 遞歸處理子節點 */
        struct dt_node *left = dt_split_recursive(
            dt, node->internal.left
        );
        struct dt_node *right = dt_split_recursive(
            dt, node->internal.right
        );
        
        /* 創建新的內部節點（COW） */
        struct dt_node *new_internal = dt_node_copy(node);
        ovsrcu_set_hidden(&new_internal->internal.left, left);
        ovsrcu_set_hidden(&new_internal->internal.right, right);
        
        return new_internal;
    }
    
    /* 葉節點：檢查是否需要分裂 */
    if (node->leaf.n_rules > dt->max_leaf_size) {
        struct dt_path dummy_path;
        dt_path_init(&dummy_path);
        return dt_split_leaf(dt, node, &dummy_path);
    }
    
    return node;
}
```

---

## 性能分析

### 增量建構時間複雜度

| 操作 | 當前實現 | 理想實現（帶分裂） |
|------|---------|-----------------|
| 插入第 1 條規則 | O(1) | O(1) |
| 插入第 n 條規則 | O(1)* | O(log n) |
| 總體插入 n 條 | O(n) | O(n log n) |
| 查找 | O(n) ❌ | O(log n) ✅ |

*注：當前實現插入是 O(1)，但查找退化為 O(n)

### 空間複雜度

**當前實現**：
```
規則數：n
節點數：1（單葉節點）
總空間：O(n)
```

**理想實現**：
```
規則數：n
葉節點數：n / max_leaf_size
內部節點數：約 (n / max_leaf_size) - 1
總空間：O(n)（相同，但結構更優）
```

### COW 開銷

**每次插入的 COW 開銷**：

```
當前實現：
  複製 1 個葉節點（包含 n 條規則的指針）
  開銷：O(1)（只複製結構體）
  
理想實現：
  複製從根到葉的路徑（約 log n 個節點）
  開銷：O(log n)
  
對比：
  n=1000 規則，樹深度約 10
  當前：複製 1 個節點
  理想：複製 10 個節點（但樹更平衡）
```

---

## 與批量建樹的對比

### 場景 1：初始加載大量規則

```
任務：加載 10,000 條規則

方案 A：批量建樹
  dt_build_tree(rules, 10000, 16);
  時間：O(n log n) ≈ 130,000 操作
  結果：最優樹結構
  
方案 B：逐條增量插入（理想實現）
  for (i = 0; i < 10000; i++) {
      dt_insert_rule(dt, rules[i]);
      // 週期性分裂
  }
  時間：O(n log n) ≈ 150,000 操作
  結果：接近最優樹結構
  
方案 C：逐條增量插入（當前實現）
  for (i = 0; i < 10000; i++) {
      dt_insert_rule(dt, rules[i]);
  }
  時間：O(n) ≈ 10,000 操作 ✅ 快！
  結果：退化樹（單葉節點）❌ 查找慢！

推薦：方案 A（批量建樹）
```

### 場景 2：運行時零星更新

```
任務：每秒添加 10 條新規則

方案 A：每次重建整棵樹
  每次插入：dt_rebuild_tree(dt);
  時間：O(n log n) 每次
  當 n=10,000：每次重建 130,000 操作 ❌ 太慢！
  
方案 B：增量插入（理想實現）
  dt_insert_rule(dt, rule);
  時間：O(log n) 每次
  當 n=10,000：每次約 13 操作 ✅
  
方案 C：增量插入 + 定期重建
  dt_insert_rule(dt, rule);
  if (insert_count % 1000 == 0) {
      dt_rebuild_tree(dt);
  }
  時間：平均 O(log n)，偶爾 O(n log n)
  效果：兼顧增量速度和樹質量 ✅

推薦：方案 C（混合策略）
```

### 場景 3：大量刪除後重新插入

```
任務：刪除 50% 規則，然後添加 30% 新規則

方案 A：維持增量更新
  for (i = 0; i < 5000; i++) {
      dt_remove_rule(dt, old_rules[i]);
  }
  for (i = 0; i < 3000; i++) {
      dt_insert_rule(dt, new_rules[i]);
  }
  問題：樹可能嚴重不平衡
  
方案 B：觸發重建
  // 刪除時檢測
  if (removed_count > n_rules * 0.3) {
      dt_rebuild_tree(dt);
  }
  效果：保持樹平衡 ✅

推薦：方案 B（檢測不平衡後重建）
```

---

## 實現建議

### 優先級 P0：修復當前插入邏輯

```c
// 位置：lib/dt-classifier.c:456-461

當前代碼：
while (node && node->type == DT_NODE_INTERNAL) {
    node = node->internal.left;  // ❌ 總是左
}

修復為：
while (node && node->type == DT_NODE_INTERNAL) {
    union mf_value value;
    mf_get_value(node->internal.field, flow, &value);
    bool match = dt_evaluate_test(node, &value);
    node = match ? node->internal.right : node->internal.left;
}
```

### 優先級 P1：增加分裂觸發機制

```c
bool
dt_insert_rule(struct decision_tree *dt, const struct cls_rule *rule,
               ovs_version_t version)
{
    // ... 現有插入邏輯 ...
    
    /* 檢查葉節點大小 */
    if (new_leaf->leaf.n_rules > dt->max_leaf_size) {
        /* 方法 1：立即分裂 */
        new_leaf = dt_split_leaf(dt, new_leaf, &path);
        
        /* 或方法 2：標記需要重建 */
        dt->needs_rebuild = true;
    }
    
    // ... 剩餘邏輯 ...
}
```

### 優先級 P2：實現定期重建

```c
/* 追蹤插入/刪除次數 */
struct decision_tree {
    // ... existing fields ...
    size_t modifications;      /* 自上次重建以來的修改次數 */
    size_t rebuild_threshold;  /* 重建觸發閾值 */
};

#define REBUILD_THRESHOLD 1000

void dt_check_rebuild(struct decision_tree *dt) {
    if (dt->modifications >= dt->rebuild_threshold) {
        /* 收集所有規則 */
        struct rculist all_rules;
        dt_collect_all_rules(dt, &all_rules);
        
        /* 重建樹 */
        struct dt_node *new_root = dt_build_tree(
            &all_rules, dt->n_rules, dt->max_leaf_size
        );
        
        /* 原子性切換 */
        struct dt_node *old_root = ovsrcu_get(&dt->root);
        ovsrcu_set(&dt->root, new_root);
        ovsrcu_postpone(dt_node_destroy, old_root);
        
        /* 重置計數器 */
        dt->modifications = 0;
    }
}
```

---

## 總結

### 當前狀態

✅ **已實現**：
- 空樹插入第一條規則
- COW 增量插入
- RCU 併發保護
- 優先級排序

❌ **未實現**：
- 樹分裂機制
- 自動重平衡
- 分裂觸發邏輯

### 增量建構流程（理想）

```
1. 初始：空樹
2. 插入規則 1-16：單葉節點
3. 插入規則 17：觸發分裂 → 樹深度 = 2
4. 繼續插入：逐步建構平衡樹
5. 定期檢查：不平衡時重建
6. 最終：O(log n) 查找性能
```

### 下一步行動

**立即修復**（2-3 小時）：
1. 修復樹遍歷邏輯（`dt_insert_rule` 和 `dt_lookup`）
2. 替換 dummy values
3. 測試驗證

**短期改進**（1-2 天）：
1. 實現 `dt_split_leaf()`
2. 添加分裂觸發機制
3. 實現定期重建策略

**長期優化**（1 周）：
1. 實現信息增益算法
2. 動態重平衡
3. 性能基準測試

---

## 參考代碼位置

- 空樹處理：`lib/dt-classifier.c:432-443`
- 增量插入：`lib/dt-classifier.c:445-502`
- COW 路徑重建：`lib/dt-classifier.c:191-224`
- 批量建樹：`lib/dt-classifier.c:702-809`
- 欄位選擇：`lib/dt-classifier.c:602-658`
- 分裂值選擇：`lib/dt-classifier.c:666-691`
