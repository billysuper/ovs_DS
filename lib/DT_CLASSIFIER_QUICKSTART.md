# Decision Tree Classifier - 快速入門指南

## API 使用範例

### 1. 初始化決策樹
```c
#include "dt-classifier.h"

struct decision_tree dt;
dt_init(&dt);
```

### 2. 創建並插入規則
```c
#include "classifier.h"
#include "openvswitch/match.h"

// 創建匹配條件
struct match match;
match_init_catchall(&match);
match_set_in_port(&match, 1);
match_set_dl_type(&match, htons(ETH_TYPE_IP));
match_set_nw_src(&match, htonl(0xc0a80000), htonl(0xffffff00)); // 192.168.0.0/24

// 創建規則
struct cls_rule *rule = xmalloc(sizeof *rule);
cls_rule_init(rule, &match, 100);  // priority = 100

// 插入到決策樹
dt_insert_rule(&dt, rule, 1);  // version = 1
```

### 3. 查找匹配規則
```c
// 創建待匹配的流
struct flow flow;
memset(&flow, 0, sizeof flow);
flow.in_port.ofp_port = 1;
flow.dl_type = htons(ETH_TYPE_IP);
flow.nw_src = htonl(0xc0a80001);  // 192.168.0.1

// 執行查找
struct flow_wildcards wc;
const struct cls_rule *found = dt_lookup(&dt, 1, &flow, &wc);

if (found) {
    printf("Found rule with priority %d\n", found->priority);
    
    // wc.masks 包含所有影響分類決策的欄位
    // 可用於 datapath 的 megaflow 安裝
}
```

### 4. 刪除規則
```c
bool removed = dt_remove_rule(&dt, rule);
if (removed) {
    printf("Rule removed successfully\n");
}

// 注意：規則本身需要 RCU 延遲釋放
ovsrcu_postpone(cls_rule_destroy, rule);
```

### 5. 獲取統計信息
```c
size_t n_rules, n_internal, n_leaf, max_depth;
dt_get_stats(&dt, &n_rules, &n_internal, &n_leaf, &max_depth);

printf("Decision Tree Statistics:\n");
printf("  Total rules: %zu\n", n_rules);
printf("  Internal nodes: %zu\n", n_internal);
printf("  Leaf nodes: %zu\n", n_leaf);
printf("  Max depth: %zu\n", max_depth);
```

### 6. 清理資源
```c
dt_destroy(&dt);
```

## 批量構建決策樹

如果有大量規則需要一次性插入，使用批量構建更高效：

```c
#include "rculist.h"

// 準備規則列表
struct rculist rules;
rculist_init(&rules);

// 添加多個規則到列表
for (int i = 0; i < 100; i++) {
    struct cls_rule *rule = create_rule(i);  // 你的規則創建函數
    rculist_push_back(&rules, &rule->node);
}

// 批量構建決策樹
size_t max_leaf_size = 10;  // 每個葉節點最多 10 個規則
struct dt_node *root = dt_build_tree(&rules, 100, max_leaf_size);

// 設置為樹的根節點
ovsrcu_set(&dt.root, root);
dt.n_rules = 100;
```

## 並發訪問範例

### 讀者端（查找）- 可並發
```c
void *reader_thread(void *arg) {
    struct decision_tree *dt = arg;
    
    while (running) {
        struct flow flow = generate_random_flow();
        struct flow_wildcards wc;
        
        // RCU 讀側臨界區
        const struct cls_rule *rule = dt_lookup(dt, current_version, &flow, &wc);
        
        if (rule) {
            process_matching_rule(rule, &wc);
        }
    }
    
    return NULL;
}
```

### 寫者端（修改）- 需互斥
```c
void *writer_thread(void *arg) {
    struct decision_tree *dt = arg;
    
    ovs_mutex_lock(&dt_mutex);  // 寫者之間需要互斥
    
    // 插入新規則
    struct cls_rule *new_rule = create_rule(...);
    dt_insert_rule(dt, new_rule, next_version);
    
    // 或刪除舊規則
    dt_remove_rule(dt, old_rule);
    
    ovs_mutex_unlock(&dt_mutex);
    
    return NULL;
}
```

## 性能優化技巧

### 1. 選擇合適的 max_leaf_size
```c
// 規則數少時
dt_build_tree(&rules, 50, 5);    // 更深的樹，查找更快

// 規則數多時
dt_build_tree(&rules, 10000, 20); // 更淺的樹，構建更快
```

### 2. 批量更新
```c
// 不好：逐個插入（每次都 COW 路徑）
for (int i = 0; i < 1000; i++) {
    dt_insert_rule(&dt, rules[i], version);  // 1000 次 COW
}

// 好：批量構建然後替換整棵樹
struct dt_node *new_tree = dt_build_tree(&all_rules, n_rules, 10);
ovsrcu_set(&dt.root, new_tree);  // 只需一次原子操作
ovsrcu_postpone(dt_node_destroy, old_root);
```

### 3. 版本管理
```c
// 避免頻繁切換版本
static ovs_version_t current_version = 1;

// 批量修改時使用未來版本
dt_insert_rule(&dt, rule1, current_version + 1);
dt_insert_rule(&dt, rule2, current_version + 1);
dt_insert_rule(&dt, rule3, current_version + 1);

// 所有修改完成後才增加版本
atomic_store(&current_version, current_version + 1);
```

## 調試技巧

### 1. 打印樹結構
```c
void print_dt_node(const struct dt_node *node, int depth) {
    if (!node) return;
    
    for (int i = 0; i < depth; i++) printf("  ");
    
    if (node->type == DT_NODE_INTERNAL) {
        printf("Internal: field=%s\n", node->internal.field->name);
        print_dt_node(ovsrcu_get(struct dt_node *, &node->internal.left), depth + 1);
        print_dt_node(ovsrcu_get(struct dt_node *, &node->internal.right), depth + 1);
    } else {
        printf("Leaf: %zu rules, %zu inherited\n",
               node->leaf.n_rules, node->leaf.n_inherited);
    }
}

// 使用
struct dt_node *root = ovsrcu_get(struct dt_node *, &dt.root);
print_dt_node(root, 0);
```

### 2. 驗證樹的一致性
```c
bool verify_dt_tree(const struct dt_node *node) {
    if (!node) return true;
    
    if (node->type == DT_NODE_INTERNAL) {
        struct dt_node *left = ovsrcu_get(struct dt_node *, &node->internal.left);
        struct dt_node *right = ovsrcu_get(struct dt_node *, &node->internal.right);
        return verify_dt_tree(left) && verify_dt_tree(right);
    } else {
        // 驗證葉節點規則列表的完整性
        const struct cls_rule *rule;
        int count = 0;
        RCULIST_FOR_EACH (rule, node, &node->leaf.rules) {
            count++;
        }
        return count == node->leaf.n_rules;
    }
}
```

### 3. 追蹤查找路徑
```c
const struct cls_rule *
dt_lookup_debug(const struct decision_tree *dt, const struct flow *flow) {
    const struct dt_node *node = ovsrcu_get(struct dt_node *, &dt.root);
    
    printf("Lookup path:\n");
    while (node && node->type == DT_NODE_INTERNAL) {
        printf("  Test field %s: ", node->internal.field->name);
        
        union mf_value value;
        mf_get_value(node->internal.field, flow, &value);
        
        bool match = evaluate_test(node, &value);
        printf("%s -> %s\n", 
               format_value(&value),
               match ? "RIGHT" : "LEFT");
        
        if (match) {
            node = ovsrcu_get(struct dt_node *, &node->internal.right);
        } else {
            node = ovsrcu_get(struct dt_node *, &node->internal.left);
        }
    }
    
    printf("  Reached leaf with %zu rules\n", node->leaf.n_rules);
    // ... 返回匹配規則
}
```

## 常見問題

### Q: 決策樹比 TSS 快嗎？
A: 取決於規則集：
- 精確匹配為主：決策樹可能更快
- 高 wildcard 規則：TSS 通常更好
- 建議：實際測試你的規則集

### Q: 如何處理 conjunction match？
A: 當前版本尚未完全實現，需要：
1. 收集所有匹配的 conjunction 組件
2. 驗證每個 conjunction set 的所有 clauses
3. 這可能需要遍歷多個葉節點

### Q: 內存使用如何？
A: 決策樹可能使用更多內存：
- 每次修改都複製路徑上的節點
- Wildcard 規則可能出現在多個葉節點（通過繼承）
- 建議：監控實際內存使用

### Q: 可以動態切換 TSS 和 DT 嗎？
A: 理論上可以，但需要：
1. 實現統一的 classifier 介面
2. 提供規則集轉換功能
3. 處理版本同步問題

## 參考資料

- OVS Classifier 文檔: `lib/classifier.h` 頭部註釋
- RCU 機制: `lib/ovs-rcu.h`
- Match 和 Flow: `include/openvswitch/match.h`, `lib/flow.h`
- 決策樹實現: `lib/dt-classifier.c`
- 完整文檔: `lib/DT_CLASSIFIER_README.md`
