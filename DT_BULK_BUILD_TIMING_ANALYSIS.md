# DT 全量建樹時機分析

## 問題

> "因為 OVS 沒有一次建構完整決策樹的功能，所以我可以在接收完所有規則後一次建構完整樹嗎？"

---

## 簡短答案

**可以，但有多種時機選擇，各有優缺點**：

| 方案 | 時機 | 優點 | 缺點 | 推薦度 |
|------|------|------|------|-------|
| **A. Bundle Commit** | OpenFlow bundle 提交時 | ✅ 原子性、批量優化 | ⚠️ 需要控制器支持 bundle | ⭐⭐⭐⭐⭐ |
| **B. 延遲建樹** | 累積 N 條規則後 | ✅ 平衡性能、簡單 | ⚠️ 需要閾值調優 | ⭐⭐⭐⭐ |
| **C. Bridge Reconfigure** | 橋接器重配置時 | ✅ 自然時機、完整重建 | ⚠️ 頻率不可控 | ⭐⭐⭐ |
| **D. 首次查找** | 第一個數據包到達時 | ✅ 懶加載、零開銷 | ⚠️ 首包延遲高 | ⭐⭐⭐ |
| **E. 後台線程** | 檢測到規則變化時 | ✅ 不阻塞數據平面 | ⚠️ 實現複雜 | ⭐⭐ |

---

## OVS 的實際規則添加模式

### 模式 1: 逐條添加 (最常見)

```bash
# 控制器或腳本逐條添加流表項
ovs-ofctl add-flow br0 "table=0, priority=100, in_port=1, actions=output:2"
ovs-ofctl add-flow br0 "table=0, priority=100, in_port=2, actions=output:1"
ovs-ofctl add-flow br0 "table=0, priority=90, actions=drop"
# ... 每條都是獨立的 OpenFlow 消息
```

**特點**:
- 每條規則立即生效
- 沒有"批量完成"信號
- 規則可能隨時增刪改

**目前 TSS 的行為**:
```c
// 每次 add-flow 調用:
classifier_insert(&table->cls, &rule->cr, version);
// 立即插入 TSS,立即可查詢
```

### 模式 2: Bundle (批量原子操作)

```bash
# 控制器使用 OpenFlow 1.4+ Bundle 機制
ovs-ofctl bundle br0 - <<EOF
flow add table=0, priority=100, in_port=1, actions=output:2
flow add table=0, priority=100, in_port=2, actions=output:1
flow add table=0, priority=90, actions=drop
flow commit
EOF
```

**特點**:
- ✅ **所有規則在 commit 時原子生效**
- ✅ **有明確的批量完成信號** (`OFPTYPE_BUNDLE_COMMIT`)
- ✅ **支持 ACID 語義** (原子性、一致性、隔離性、持久性)

**Bundle 執行流程**:
```c
// 階段 1: 收集所有規則 (不生效)
OFPTYPE_BUNDLE_ADD_MESSAGE → handle_bundle_add()
  → ofp_bundle_add_message()
  → 存儲到 bundle->msg_list (不插入 classifier)

// 階段 2: 驗證所有規則
OFPTYPE_BUNDLE_COMMIT → do_bundle_commit()
  → LIST_FOR_EACH(be, &bundle->msg_list) {
      ofproto_flow_mod_start(ofproto, &be->ofm);  // 驗證但不發布
    }

// 階段 3: 原子發布 (關鍵!)
  → ofproto_publish_classifiers(ofproto);  // 一次性發布所有變更
  
// 階段 4: 完成
  → LIST_FOR_EACH(be, &bundle->msg_list) {
      ofproto_flow_mod_finish(ofproto, &be->ofm);
    }
```

### 模式 3: 重配置 (Bridge Reconfigure)

```bash
# OVSDB 數據庫變更觸發
ovs-vsctl set-controller br0 tcp:127.0.0.1:6653
# → 觸發 bridge_reconfigure()
# → 可能重建所有流表
```

**特點**:
- 系統級重啟或配置變更時觸發
- 頻率較低 (分鐘級)
- 可能清空並重建流表

---

## 方案 A: Bundle Commit 時建樹 ⭐⭐⭐⭐⭐ (推薦)

### 實現方式

```c
// lib/dt-classifier.c

// 標記 DT 為"批量模式"
void dt_begin_batch(struct decision_tree *dt) {
    dt->batch_mode = true;
    dt->pending_rules = NULL;
    dt->n_pending = 0;
}

// 累積規則,不立即建樹
void dt_insert_rule_batch(struct decision_tree *dt, const struct cls_rule *rule) {
    ovs_assert(dt->batch_mode);
    
    // 暫存到待處理列表
    dt->pending_rules = xrealloc(dt->pending_rules, 
                                  (dt->n_pending + 1) * sizeof(struct cls_rule *));
    dt->pending_rules[dt->n_pending++] = rule;
    
    // 不建樹!
}

// Commit 時一次性建樹
void dt_commit_batch(struct decision_tree *dt) {
    ovs_assert(dt->batch_mode);
    
    if (dt->n_pending > 0) {
        // 合併舊規則 + 新規則
        struct cls_rule **all_rules = collect_all_rules(dt);
        size_t n_all = dt->n_rules + dt->n_pending;
        
        // 一次性建樹
        struct dt_node *new_root = dt_build_tree(all_rules, n_all);
        
        // 原子替換 (COW)
        ovsrcu_postpone(free_tree, dt->root);
        ovsrcu_set(&dt->root, new_root);
        
        dt->n_rules = n_all;
    }
    
    dt->batch_mode = false;
    free(dt->pending_rules);
    dt->pending_rules = NULL;
    dt->n_pending = 0;
}
```

### 集成到 Classifier

```c
// lib/classifier.c

void classifier_begin_batch(struct classifier *cls) {
    if (cls->backend_type == CLASSIFIER_BACKEND_DT) {
        dt_begin_batch(&cls->dt);
    }
    // TSS 不需要特殊處理
}

void classifier_insert_batch(struct classifier *cls, struct cls_rule *rule) {
    if (cls->backend_type == CLASSIFIER_BACKEND_DT) {
        dt_insert_rule_batch(&cls->dt, rule);
    } else {
        classifier_insert(&cls->tss, rule);  // TSS 照常
    }
}

void classifier_commit_batch(struct classifier *cls) {
    if (cls->backend_type == CLASSIFIER_BACKEND_DT) {
        dt_commit_batch(&cls->dt);
    }
}
```

### 修改 Bundle 處理邏輯

```c
// ofproto/ofproto.c

static enum ofperr
do_bundle_commit(struct ofconn *ofconn, uint32_t id, uint16_t flags)
{
    struct ofproto *ofproto = ofconn_get_ofproto(ofconn);
    struct ofp_bundle *bundle = ofconn_get_bundle(ofconn, id);
    
    // *** 新增: 啟動批量模式 ***
    for (int i = 0; i < ofproto->n_tables; i++) {
        classifier_begin_batch(&ofproto->tables[i].cls);
    }
    
    // 原有邏輯: 添加所有規則
    LIST_FOR_EACH(be, node, &bundle->msg_list) {
        if (be->type == OFPTYPE_FLOW_MOD) {
            ofproto_flow_mod_start(ofproto, &be->ofm);
            // 規則被添加到 pending_rules,不建樹
        }
    }
    
    // *** 新增: 提交批量變更 ***
    for (int i = 0; i < ofproto->n_tables; i++) {
        classifier_commit_batch(&ofproto->tables[i].cls);  // 在這裡建樹!
    }
    
    // 發布變更
    ofproto_publish_classifiers(ofproto);
    
    // 完成
    LIST_FOR_EACH(be, node, &bundle->msg_list) {
        ofproto_flow_mod_finish(ofproto, &be->ofm);
    }
}
```

### 優點

- ✅ **真正的批量優化**: 一次建樹,包含所有規則
- ✅ **原子性**: 符合 OpenFlow bundle 語義
- ✅ **兼容性好**: 不影響非 bundle 操作
- ✅ **性能最優**: 避免增量建樹的多次重建

### 缺點

- ⚠️ 需要控制器支持 OpenFlow 1.4+ bundle
- ⚠️ 非 bundle 操作仍需增量插入

---

## 方案 B: 延遲建樹 (閾值觸發) ⭐⭐⭐⭐

### 實現方式

```c
// lib/dt-classifier.c

#define DT_REBUILD_THRESHOLD 100  // 累積 100 條規則後重建

struct decision_tree {
    struct dt_node *root;
    size_t n_rules;
    size_t n_rules_since_rebuild;  // 距離上次重建新增的規則數
    bool needs_rebuild;
};

void dt_insert_rule_simple(struct decision_tree *dt, const struct cls_rule *rule) {
    // 增量插入 (COW)
    dt_insert_incremental(dt, rule);
    dt->n_rules++;
    dt->n_rules_since_rebuild++;
    
    // 檢查是否需要重建
    if (dt->n_rules_since_rebuild >= DT_REBUILD_THRESHOLD) {
        dt->needs_rebuild = true;
    }
}

// 週期性調用 (或在查找前調用)
void dt_maybe_rebuild(struct decision_tree *dt) {
    if (!dt->needs_rebuild) {
        return;
    }
    
    // 收集所有規則
    struct cls_rule **all_rules = dt_collect_all_rules(dt);
    
    // 重建樹
    struct dt_node *new_root = dt_build_tree(all_rules, dt->n_rules);
    
    // 原子替換
    ovsrcu_postpone(free_tree, dt->root);
    ovsrcu_set(&dt->root, new_root);
    
    dt->n_rules_since_rebuild = 0;
    dt->needs_rebuild = false;
    
    free(all_rules);
}

// 查找時觸發重建
const struct cls_rule *dt_lookup(struct decision_tree *dt, const struct flow *flow) {
    // 先檢查是否需要重建
    dt_maybe_rebuild(dt);
    
    // 然後查找
    return dt_lookup_node(dt->root, flow);
}
```

### 優點

- ✅ **簡單**: 不依賴 bundle 機制
- ✅ **自動優化**: 自動在合適時機重建
- ✅ **兼容性好**: 對外部透明

### 缺點

- ⚠️ 閾值難以調優 (太小頻繁重建,太大性能差)
- ⚠️ 重建時機不可控
- ⚠️ 可能在查找時觸發重建 (延遲抖動)

---

## 方案 C: Bridge Reconfigure 時建樹 ⭐⭐⭐

### 實現方式

```c
// vswitchd/bridge.c

void bridge_reconfigure(const struct ovsrec_open_vswitch *ovs_cfg)
{
    // ... 原有邏輯 ...
    
    // 重配置所有橋接器的流表
    struct bridge *br;
    HMAP_FOR_EACH(br, node, &all_bridges) {
        struct ofproto *ofproto = br->ofproto;
        
        // 重建所有表的決策樹
        for (int i = 0; i < ofproto->n_tables; i++) {
            classifier_rebuild(&ofproto->tables[i].cls);
        }
    }
}
```

```c
// lib/classifier.c

void classifier_rebuild(struct classifier *cls) {
    if (cls->backend_type == CLASSIFIER_BACKEND_DT) {
        struct cls_rule **all_rules = collect_all_rules(&cls->dt);
        
        struct dt_node *new_root = dt_build_tree(all_rules, cls->dt.n_rules);
        
        ovsrcu_postpone(free_tree, cls->dt.root);
        ovsrcu_set(&cls->dt.root, new_root);
        
        free(all_rules);
    }
}
```

### 優點

- ✅ **自然時機**: 系統級重配置時重建
- ✅ **完整重建**: 可以優化整個樹結構

### 缺點

- ⚠️ 觸發頻率不可控 (可能很少觸發)
- ⚠️ 依賴外部事件

---

## 方案 D: 首次查找時建樹 (懶加載) ⭐⭐⭐

### 實現方式

```c
// lib/dt-classifier.c

struct decision_tree {
    struct dt_node *root;
    struct cls_rule **pending_rules;  // 待建樹的規則
    size_t n_pending;
    bool tree_built;
};

void dt_insert_rule_simple(struct decision_tree *dt, const struct cls_rule *rule) {
    // 只是累積規則,不建樹
    dt->pending_rules = xrealloc(dt->pending_rules, 
                                  (dt->n_pending + 1) * sizeof(*dt->pending_rules));
    dt->pending_rules[dt->n_pending++] = rule;
    dt->tree_built = false;
}

const struct cls_rule *dt_lookup(struct decision_tree *dt, const struct flow *flow) {
    // 首次查找時建樹
    if (!dt->tree_built && dt->n_pending > 0) {
        dt->root = dt_build_tree(dt->pending_rules, dt->n_pending);
        dt->tree_built = true;
        free(dt->pending_rules);
        dt->pending_rules = NULL;
    }
    
    return dt_lookup_node(dt->root, flow);
}
```

### 優點

- ✅ **零開銷**: 如果沒有查找,不建樹
- ✅ **簡單**: 實現簡單

### 缺點

- ⚠️ **首包延遲高**: 第一個數據包會觸發建樹 (可能幾十毫秒)
- ⚠️ **不適合生產**: 首包延遲不可接受

---

## 方案 E: 後台線程建樹 ⭐⭐

### 實現方式

```c
// lib/dt-classifier.c

struct decision_tree {
    struct dt_node *root;           // 當前生效的樹
    struct dt_node *building_tree;  // 正在構建的樹
    struct ovs_mutex build_mutex;
    pthread_t build_thread;
    bool rebuild_requested;
};

// 規則變化時請求重建
void dt_insert_rule_simple(struct decision_tree *dt, const struct cls_rule *rule) {
    // 增量插入到當前樹
    dt_insert_incremental(dt, rule);
    
    // 標記需要重建
    ovs_mutex_lock(&dt->build_mutex);
    dt->rebuild_requested = true;
    ovs_mutex_unlock(&dt->build_mutex);
}

// 後台線程
void *dt_builder_thread(void *dt_) {
    struct decision_tree *dt = dt_;
    
    while (true) {
        ovs_mutex_lock(&dt->build_mutex);
        if (dt->rebuild_requested) {
            dt->rebuild_requested = false;
            ovs_mutex_unlock(&dt->build_mutex);
            
            // 收集所有規則
            struct cls_rule **rules = dt_collect_all_rules(dt);
            
            // 構建新樹 (可能花費幾十毫秒)
            struct dt_node *new_tree = dt_build_tree(rules, dt->n_rules);
            
            // 原子替換
            ovsrcu_postpone(free_tree, dt->root);
            ovsrcu_set(&dt->root, new_tree);
            
            free(rules);
        } else {
            ovs_mutex_unlock(&dt->build_mutex);
            poll_timer_wait(100);  // 100ms 檢查一次
        }
    }
}
```

### 優點

- ✅ **不阻塞數據平面**: 查找不受影響
- ✅ **自動優化**: 後台持續優化樹結構

### 缺點

- ⚠️ **實現複雜**: 需要線程同步
- ⚠️ **資源開銷**: 額外線程和內存
- ⚠️ **調試困難**: 並發問題

---

## 推薦方案

### 短期實現 (MVP)

**方案 B (延遲建樹)** + **方案 D (懶加載)**

```c
// 結合兩者優點
const struct cls_rule *dt_lookup(struct decision_tree *dt, const struct flow *flow) {
    // 懶加載: 首次查找時建樹
    if (!dt->tree_built && dt->n_pending > 0) {
        dt->root = dt_build_tree(dt->pending_rules, dt->n_pending);
        dt->tree_built = true;
    }
    
    // 延遲重建: 累積到閾值後重建
    if (dt->needs_rebuild) {
        dt_maybe_rebuild(dt);
    }
    
    return dt_lookup_node(dt->root, flow);
}
```

**優點**:
- 實現簡單 (1-2 小時)
- 初始可用 (解決首次建樹問題)
- 支持增量更新

### 長期優化 (生產級)

**方案 A (Bundle Commit)** 為主 + **方案 B (延遲建樹)** 為輔

```c
// 優先使用 bundle
void classifier_insert(struct classifier *cls, struct cls_rule *rule) {
    if (cls->in_batch_mode) {
        // Bundle 模式: 累積規則
        dt_insert_rule_batch(&cls->dt, rule);
    } else {
        // 非 Bundle 模式: 延遲重建
        dt_insert_rule_simple(&cls->dt, rule);
        
        if (cls->dt.n_rules_since_rebuild >= DT_REBUILD_THRESHOLD) {
            dt_maybe_rebuild(&cls->dt);
        }
    }
}
```

**優點**:
- Bundle 操作獲得最佳性能
- 非 Bundle 操作也有優化
- 兼容所有使用場景

---

## 測試策略

### 測試 1: Bundle 批量添加

```bash
# 測試 bundle 建樹
ovs-ofctl bundle br0 - <<EOF
$(for i in {1..1000}; do
    echo "flow add table=0, priority=$i, in_port=$((i%4+1)), actions=output:$((i%4+2))"
done)
flow commit
EOF

# 驗證: 應該只建一次樹
ovs-appctl dpctl/dump-flows | wc -l  # 1000 條規則
```

### 測試 2: 逐條添加 + 延遲重建

```bash
# 測試增量插入 + 閾值重建
for i in {1..1000}; do
    ovs-ofctl add-flow br0 "table=0, priority=$i, in_port=$((i%4+1)), actions=output:$((i%4+2))"
done

# 驗證: 應該重建 1000/100 = 10 次樹
```

### 測試 3: 首次查找建樹

```bash
# 添加規則但不查找
for i in {1..100}; do
    ovs-ofctl add-flow br0 "table=0, priority=$i, actions=drop"
done

# 樹應該還沒建

# 發送數據包觸發查找
ovs-appctl ofproto/trace br0 in_port=1

# 樹應該在第一次 trace 時建立
```

---

## 代碼實現示例

### 完整的 dt_insert_rule_smart()

```c
// lib/dt-classifier.c

enum dt_insert_mode {
    DT_INSERT_INCREMENTAL,  // 增量插入 (COW)
    DT_INSERT_BATCH,        // 批量累積
    DT_INSERT_LAZY          // 懶加載
};

struct decision_tree {
    struct dt_node *root;
    struct cls_rule **all_rules;
    size_t n_rules;
    size_t capacity;
    
    // 批量模式
    bool batch_mode;
    struct cls_rule **pending_rules;
    size_t n_pending;
    
    // 延遲重建
    size_t n_rules_since_rebuild;
    bool needs_rebuild;
    
    // 懶加載
    bool tree_built;
    
    enum dt_insert_mode mode;
};

void dt_insert_rule_smart(struct decision_tree *dt, const struct cls_rule *rule) {
    switch (dt->mode) {
        case DT_INSERT_BATCH:
            // Bundle 模式: 累積規則
            dt->pending_rules[dt->n_pending++] = rule;
            break;
            
        case DT_INSERT_LAZY:
            // 懶加載模式: 只累積,不建樹
            dt->all_rules[dt->n_rules++] = rule;
            dt->tree_built = false;
            break;
            
        case DT_INSERT_INCREMENTAL:
        default:
            // 增量模式: 立即插入 + 延遲重建
            dt_insert_incremental(dt, rule);
            dt->n_rules++;
            dt->n_rules_since_rebuild++;
            
            if (dt->n_rules_since_rebuild >= DT_REBUILD_THRESHOLD) {
                dt->needs_rebuild = true;
            }
            break;
    }
}

const struct cls_rule *dt_lookup_smart(struct decision_tree *dt, 
                                       const struct flow *flow) {
    // 懶加載: 首次查找時建樹
    if (!dt->tree_built && dt->n_rules > 0) {
        dt->root = dt_build_tree(dt->all_rules, dt->n_rules);
        dt->tree_built = true;
    }
    
    // 延遲重建: 檢查是否需要優化
    if (dt->needs_rebuild) {
        dt_maybe_rebuild(dt);
    }
    
    // 正常查找
    return dt_lookup_node(dt->root, flow);
}

void dt_commit_batch_smart(struct decision_tree *dt) {
    if (dt->batch_mode && dt->n_pending > 0) {
        // 合併所有規則
        memcpy(dt->all_rules + dt->n_rules, dt->pending_rules, 
               dt->n_pending * sizeof(struct cls_rule *));
        dt->n_rules += dt->n_pending;
        
        // 一次性建樹
        dt->root = dt_build_tree(dt->all_rules, dt->n_rules);
        dt->tree_built = true;
        
        // 清理
        dt->n_pending = 0;
        dt->batch_mode = false;
    }
}
```

---

## 總結

| 方案 | 建樹時機 | 適用場景 | 實現複雜度 |
|------|---------|---------|-----------|
| **A. Bundle** | Bundle commit | 控制器批量操作 | 中等 |
| **B. 延遲建樹** | 累積閾值 | 通用場景 | 低 |
| **C. Reconfigure** | 系統重配置 | 初始化、重啟 | 低 |
| **D. 懶加載** | 首次查找 | 測試、輕量場景 | 極低 |
| **E. 後台線程** | 持續優化 | 高性能場景 | 高 |

**推薦實現路徑**:
1. **第一階段 (MVP)**: 實現方案 D (懶加載) - 1 小時
2. **第二階段 (優化)**: 添加方案 B (延遲重建) - 2 小時
3. **第三階段 (生產)**: 添加方案 A (Bundle 支持) - 4 小時

**核心答案**:
> ✅ **可以在接收完所有規則後一次建樹**，最佳方案是利用 **OpenFlow Bundle Commit** 時機，既符合原子語義，又能獲得最佳性能！
