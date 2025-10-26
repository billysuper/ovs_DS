# TSS (Tuple Space Search) 分類機制詳解

## 目錄
1. [TSS 核心概念](#tss-核心概念)
2. [資料結構設計](#資料結構設計)
3. [欄位利用策略](#欄位利用策略)
4. [查找演算法](#查找演算法)
5. [優化機制](#優化機制)
6. [與 DT 的對比](#與-dt-的對比)

---

## TSS 核心概念

### 什麼是 Tuple Space Search？

TSS 將規則按照 **mask 模式（tuple）** 分組，相同 mask 的規則放在同一個 **subtable** 中。

```
核心思想：
┌─────────────────────────────────────────────────────┐
│  "具有相同 wildcard 模式的規則會匹配相同的欄位"   │
│                                                     │
│  例如：                                             │
│  Rule1: ip_dst=10.0.1.5/32, priority=100            │
│  Rule2: ip_dst=10.0.2.8/32, priority=90             │
│  → 都使用 mask: 0xFFFFFFFF (32 位元全匹配)         │
│  → 放在同一個 subtable                              │
└─────────────────────────────────────────────────────┘
```

### 為什麼叫 "Tuple Space"？

**Tuple = (mask, flow)**
- **mask**：定義哪些欄位參與匹配（tuple 的維度）
- **flow**：具體的匹配值

```
範例：
┌──────────────────────────────────────────────────────┐
│ Tuple 1: (mask: ip_dst=0xFFFFFFFF)                   │
│   - Rule1: ip_dst=10.0.1.1, priority=100             │
│   - Rule2: ip_dst=10.0.1.2, priority=90              │
│                                                       │
│ Tuple 2: (mask: ip_dst=0xFFFFFF00, tp_dst=0xFFFF)    │
│   - Rule3: ip_dst=10.0.2.0/24, tp_dst=80, pri=100    │
│   - Rule4: ip_dst=10.0.3.0/24, tp_dst=443, pri=90    │
│                                                       │
│ Tuple 3: (mask: in_port=0xFFFF, eth_type=0xFFFF)     │
│   - Rule5: in_port=1, eth_type=0x0800, pri=80        │
│   - Rule6: in_port=2, eth_type=0x86dd, pri=80        │
└──────────────────────────────────────────────────────┘
```

---

## 資料結構設計

### 1. 整體架構

```c
struct classifier {
    struct cmap subtables_map;        // 所有 subtables (按 mask 分組)
    struct pvector subtables;         // 按優先級排序的 subtables
    struct cls_trie tries[CLS_MAX_TRIES];  // 前綴樹優化（IP 地址）
    atomic_uint n_tries;              // 啟用的 trie 數量
    int n_rules;                      // 總規則數
    bool publish;                     // 是否發布更新
};
```

**關鍵點**：
- `subtables_map`：用 **mask** 作為 key，查找對應的 subtable
- `subtables`：按 **最高優先級** 排序，查找時從高到低遍歷
- `tries`：針對 **IP 前綴匹配** 的額外優化

---

### 2. Subtable（子表）

**定義**：`lib/classifier-private.h:29-55`

```c
struct cls_subtable {
    struct cmap_node cmap_node;       // 在 classifier->subtables_map 中
    
    /* 寫入者使用 */
    int max_priority;                 // 該 subtable 中的最高優先級
    unsigned int max_count;           // 最高優先級規則的數量
    
    /* 迭代器使用 */
    struct rculist rules_list;        // 無序規則列表
    
    /* 讀者關心 wildcarding 的字段 */
    const uint8_t n_indices;          // 使用多少個哈希索引
    const struct flowmap index_maps[CLS_MAX_INDICES + 1]; // 階段映射
    unsigned int trie_plen[CLS_MAX_TRIES];  // Trie 前綴長度
    const int ports_mask_len;         // 端口 mask 長度
    struct ccmap indices[CLS_MAX_INDICES];  // 階段查找索引
    rcu_trie_ptr ports_trie;          // 端口前綴樹
    
    /* 所有讀者訪問 */
    struct cmap rules;                // 包含 cls_match（規則的內部表示）
    const struct minimask mask;       // 該 subtable 的 wildcard mask
};
```

**關鍵欄位**：

| 欄位 | 用途 | 說明 |
|------|------|------|
| `mask` | **定義 tuple** | 所有規則共享的 wildcard 模式 |
| `rules` | **規則存儲** | cmap（並發哈希表）存儲實際規則 |
| `indices[]` | **多階段哈希** | 加速規則查找（staged lookup） |
| `trie_plen[]` | **前綴優化** | IP 前綴長度，用於跳過不匹配的 subtable |
| `ports_trie` | **端口優化** | TCP/UDP 端口前綴樹 |
| `max_priority` | **優先級排序** | 決定 subtable 查找順序 |

---

### 3. Cls_match（規則）

**定義**：`lib/classifier-private.h:57-84`

```c
struct cls_match {
    /* 所有人訪問 */
    OVSRCU_TYPE(struct cls_match *) next;  // 相同匹配但低優先級的規則
    OVSRCU_TYPE(struct cls_conjunction_set *) conj_set;  // Conjunction 集合
    
    /* 關心 wildcarding 的讀者 */
    const int priority;               // 優先級（越大越高）
    
    /* 所有讀者 */
    struct cmap_node cmap_node;       // 在 subtable->rules 中
    struct versions versions;         // 版本控制（MVCC）
    const struct cls_rule *cls_rule;  // 指向外部規則
    const struct miniflow flow;       // 匹配值（mask 在 subtable 中）
};
```

**關鍵點**：
- **miniflow**：壓縮的 flow 表示，只存儲 **非 wildcard 的欄位**
- **mask 不存儲**：mask 存在 subtable 中，所有規則共享
- **優先級鏈**：`next` 指向相同 flow+mask 但低優先級的規則

---

### 4. Minimask & Miniflow（壓縮表示）

#### 為什麼需要壓縮？

```
struct flow: 240 bytes (包含所有 173 個欄位)
  ↓ 大部分欄位是 wildcarded（不關心）
miniflow: 只存儲非 wildcard 的欄位
  → 節省內存和計算
```

#### Miniflow 結構

```c
struct miniflow {
    struct flowmap map;       // Bitmap 指示哪些欄位有值
    uint64_t values[];        // 緊湊存儲的欄位值
};
```

**範例**：
```
原始規則: ip_dst=10.0.1.5, tp_dst=80
struct flow (240 bytes):
  dl_dst = 0, dl_src = 0, dl_type = 0, ..., 
  nw_dst = 0x0A000105,    ← 有值
  ..., 
  tp_dst = 80,            ← 有值
  ...

miniflow (24 bytes):
  map = 0x...00100...01000  (bit 23=1, bit 56=1)
  values[0] = 0x0A000105    (nw_dst)
  values[1] = 80            (tp_dst)
```

#### Minimask 結構

```c
struct minimask {
    struct miniflow masks;    // 存儲 mask 值（非 0 的 mask 位）
};
```

**範例**：
```
mask: ip_dst=0xFFFFFFFF, tp_dst=0xFFFF
minimask:
  map = 0x...00100...01000
  values[0] = 0xFFFFFFFF
  values[1] = 0xFFFF
```

---

## 欄位利用策略

### TSS 如何利用所有欄位？

**答案：完全透明！所有 173 個欄位都可以用於分類。**

TSS 不需要預先指定候選欄位，任何欄位組合都可以：

```c
// 支持任意欄位組合
Rule1: in_port=1, eth_type=0x0800              → Tuple 1
Rule2: ip_dst=10.0.1.5/32                      → Tuple 2
Rule3: ipv6_src=2001:db8::1/128, tp_dst=80     → Tuple 3
Rule4: tun_id=100, ct_state=+trk+new           → Tuple 4
Rule5: vlan_vid=100, mpls_label=500            → Tuple 5
Rule6: nsh_spi=42, reg0=0x12345                → Tuple 6

→ 每種 mask 組合自動創建一個 subtable
```

### 欄位使用統計

**代碼位置**：`lib/classifier.c:183-207`

```c
void
cls_rule_init(struct cls_rule *rule, const struct match *match, int priority)
{
    cls_rule_init__(rule, priority);
    minimatch_init(CONST_CAST(struct minimatch *, &rule->match), match);
    //         ↑
    //         自動根據 match->wc（wildcards）創建 minimask
}
```

**工作流程**：
1. 用戶創建規則，指定 `match`（包含 flow + wildcards）
2. TSS 從 `wildcards` 自動提取 mask
3. 根據 mask 查找或創建 subtable
4. 將規則插入該 subtable

**完全自動化！無需人工指定欄位。**

---

## 查找演算法

### 核心流程

**代碼位置**：`lib/classifier.c:958-1098` (`classifier_lookup__`)

```c
const struct cls_rule *
classifier_lookup__(const struct classifier *cls, ovs_version_t version,
                    struct flow *flow, struct flow_wildcards *wc,
                    bool allow_conjunctive_matches,
                    struct hmapx *conj_flows)
{
    // 第一步：初始化 Trie 上下文（IP 前綴優化）
    struct trie_ctx trie_ctx[CLS_MAX_TRIES];
    for (uint32_t i = 0; i < n_tries; i++) {
        trie_ctx_init(&trie_ctx[i], &cls->tries[i]);
    }
    
    // 第二步：遍歷所有 subtables（按優先級從高到低）
    const struct cls_subtable *subtable;
    PVECTOR_FOR_EACH_PRIORITY (subtable, hard_pri, 2, sizeof *subtable,
                                &cls->subtables) {
        
        // 第三步：在 subtable 內查找匹配規則
        match = find_match_wc(subtable, version, flow, 
                              trie_ctx, n_tries, wc);
        
        if (match) {
            // 第四步：處理優先級和 conjunction
            if (OVS_LIKELY(!match->conj_set)) {
                return match->cls_rule;  // 找到！
            } else {
                // Conjunction 處理...
            }
        }
    }
    
    return NULL;  // 沒找到
}
```

---

### 詳細步驟

#### 步驟 1：Trie 優化（可選）

**目的**：快速跳過不可能匹配的 subtables

**代碼位置**：`lib/classifier.c:999-1004`

```c
// 初始化 trie contexts
for (uint32_t i = 0; i < n_tries; i++) {
    trie_ctx_init(&trie_ctx[i], &cls->tries[i]);
}
```

**原理**：
```
假設封包：ip_dst = 10.0.1.5

Subtable A: mask = 10.0.0.0/16  (16-bit prefix)
Subtable B: mask = 192.168.0.0/16
Subtable C: mask = 10.0.1.0/24  (24-bit prefix)

Trie 查找結果：
  - 16-bit prefix: 10.0.x.x → 可能匹配 A ✓
  - 24-bit prefix: 10.0.1.x → 可能匹配 C ✓
  - 沒有 192.168 的前綴 → 跳過 B ✗
```

**數據結構**：
```c
struct cls_trie {
    const struct mf_field *field;  // 字段（如 MFF_IPV4_DST）
    rcu_trie_ptr root;             // 前綴樹根節點
};

struct trie_ctx {
    const struct cls_trie *trie;
    bool lookup_done;              // 是否已查找
    uint8_t be32ofs;               // 字段在 flow 中的偏移
    unsigned int maskbits;         // 需要的前綴長度
    union trie_prefix match_plens; // 可能匹配的前綴長度位圖
};
```

---

#### 步驟 2：遍歷 Subtables（按優先級）

**代碼位置**：`lib/classifier.c:1005-1015`

```c
PVECTOR_FOR_EACH_PRIORITY (subtable, hard_pri, 2, sizeof *subtable,
                            &cls->subtables) {
    // subtable 按 max_priority 從高到低排序
    // hard_pri 是當前已找到的最高優先級
    
    // 如果 subtable->max_priority <= hard_pri，
    // 後續 subtables 都不可能更好，可以提前退出
    if (subtable->max_priority <= hard_pri) {
        break;
    }
    
    match = find_match_wc(subtable, version, flow, 
                          trie_ctx, n_tries, wc);
    // ...
}
```

**優化機制**：
1. **優先級排序**：subtables 按 `max_priority` 排序
2. **提前終止**：找到匹配後，跳過低優先級 subtables
3. **Trie 跳過**：使用前綴樹判斷是否需要檢查某個 subtable

---

#### 步驟 3：Subtable 內查找

**代碼位置**：`lib/classifier.c:1720-1890` (`find_match_wc`)

```c
static const struct cls_match *
find_match_wc(const struct cls_subtable *subtable, ovs_version_t version,
              const struct flow *flow, struct trie_ctx *trie_ctx,
              uint32_t n_tries, struct flow_wildcards *wc)
{
    // 3.1 檢查 Trie（IP 前綴）是否匹配
    if (OVS_UNLIKELY(!trie_ctx->lookup_done)) {
        // 在 trie 中查找前綴
        lookup_res = trie_lookup(&trie_ctx->trie, flow, &plens);
        
        // 檢查 subtable 的前綴長度是否在可能匹配的列表中
        unsigned int plen = subtable->trie_plen[i];
        if (plen && !be_get_bit_at(&plens, plen - 1)) {
            return NULL;  // 不匹配，跳過此 subtable
        }
    }
    
    // 3.2 檢查端口 Trie（TCP/UDP 端口）
    if (subtable->ports_trie) {
        ovs_be32 ports = miniflow_get_ports(flow);
        if (!trie_lookup_prefix(&subtable->ports_trie, &ports,
                                subtable->ports_mask_len)) {
            return NULL;  // 端口不匹配，跳過
        }
    }
    
    // 3.3 多階段哈希查找（Staged Lookup）
    uint32_t basis = 0, hash = 0;
    uint8_t n_indices = subtable->n_indices;
    
    for (uint8_t i = 0; i < n_indices; i++) {
        // 計算階段哈希
        hash = flow_hash_in_minimask_range(flow, &subtable->mask,
                                           subtable->index_maps[i],
                                           &basis);
        
        // 檢查哈希索引
        if (!ccmap_find(&subtable->indices[i], hash)) {
            return NULL;  // 該階段無匹配，跳過
        }
    }
    
    // 3.4 最終哈希查找
    hash = flow_hash_in_minimask_range(flow, &subtable->mask,
                                       subtable->index_maps[n_indices],
                                       &basis);
    
    // 3.5 在規則表中查找
    const struct cls_match *rule;
    CMAP_FOR_EACH_WITH_HASH (rule, cmap_node, hash, &subtable->rules) {
        // 完整匹配檢查
        if (miniflow_and_mask_matches_flow(&rule->flow, &subtable->mask, flow)
            && cls_match_visible_in_version(rule, version)) {
            return rule;  // 找到匹配！
        }
    }
    
    return NULL;  // 未找到
}
```

---

### 欄位匹配的核心函數

**代碼位置**：`lib/flow.c` (`miniflow_and_mask_matches_flow`)

```c
bool
miniflow_and_mask_matches_flow(const struct miniflow *flow,
                                const struct minimask *mask,
                                const struct flow *target)
{
    const uint64_t *flowp = miniflow_get_values(flow);
    const uint64_t *maskp = miniflow_get_values(&mask->masks);
    const uint64_t *target_u64 = (const uint64_t *)target;
    
    // 遍歷所有非 wildcard 的欄位
    map_t map;
    FLOWMAP_FOR_EACH_INDEX(idx, map, flowmap_or(&flow->map, &mask->masks.map)) {
        // 檢查每個欄位是否匹配
        if (((flowp[idx] ^ target_u64[idx]) & maskp[idx]) != 0) {
            return false;  // 不匹配
        }
    }
    
    return true;  // 所有欄位都匹配
}
```

**核心邏輯**：
```
對於每個非 wildcard 的欄位：
  (rule_value XOR packet_value) AND mask == 0
  
範例：
  rule_value = 10.0.1.5  (0x0A000105)
  packet_value = 10.0.1.5  (0x0A000105)
  mask = 0xFFFFFFFF
  
  (0x0A000105 XOR 0x0A000105) AND 0xFFFFFFFF = 0 ✓ 匹配
  
  如果 packet_value = 10.0.1.6:
  (0x0A000105 XOR 0x0A000106) AND 0xFFFFFFFF = 0x00000003 ✗ 不匹配
```

---

## 優化機制

### 1. Trie（前綴樹）優化

**目的**：快速跳過不匹配 IP 前綴的 subtables

**支持的欄位**：
```c
// lib/classifier.c:353-387 (classifier_set_prefix_fields)
支持任意數量的 prefix 字段，常見的有：
- MFF_IPV4_SRC
- MFF_IPV4_DST
- MFF_IPV6_SRC
- MFF_IPV6_DST
```

**數據結構**：
```c
struct cls_trie {
    const struct mf_field *field;  // 如 MFF_IPV4_DST
    rcu_trie_ptr root;             // Patricia Trie 根節點
};
```

**原理**：
```
Trie 結構：
              [root]
             /      \
        10.0.0.0/8   192.168.0.0/16
          /    \
    10.0.0.0/16  10.1.0.0/16
        /
  10.0.1.0/24

查找 10.0.1.5:
  1. 檢查 8-bit: 10.x.x.x ✓
  2. 檢查 16-bit: 10.0.x.x ✓
  3. 檢查 24-bit: 10.0.1.x ✓
  
結果：可能匹配 8, 16, 24 bit 前綴的 subtables
     不可能匹配 192.168.x.x 的 subtables
```

**性能影響**：
- **最好情況**：跳過 90% 的 subtables
- **最壞情況**：無法跳過（所有 subtables 都可能匹配）

---

### 2. Ports Trie（端口前綴樹）

**目的**：快速跳過不匹配 TCP/UDP 端口的規則

**支持的欄位**：
```c
// 自動支持
MFF_TCP_SRC, MFF_TCP_DST, MFF_UDP_SRC, MFF_UDP_DST
```

**原理**：
```
tp_src 和 tp_dst 共 32 bits：
  [tp_src:16 bits][tp_dst:16 bits]

Ports Trie:
       [root]
      /      \
   80:*    443:*
   
查找 tp_dst=80:
  → 快速確認是否有 tp_dst=80 的規則
```

---

### 3. Staged Lookup（多階段哈希）

**目的**：減少哈希衝突，加速規則查找

**代碼位置**：`lib/classifier-private.h:40-50`

```c
struct cls_subtable {
    const uint8_t n_indices;      // 階段數（通常 2-3）
    const struct flowmap index_maps[CLS_MAX_INDICES + 1];  // 每階段使用的欄位
    struct ccmap indices[CLS_MAX_INDICES];  // 每階段的哈希表
};
```

**原理**：
```
假設 subtable 有 3 個階段：

階段 1：只用 in_port
  → 計算 hash1 = hash(flow->in_port)
  → 檢查 indices[0][hash1] 是否存在
  → 如果不存在，說明沒有匹配的規則，提前返回

階段 2：只用 eth_type, nw_proto
  → 計算 hash2 = hash(flow->eth_type, flow->nw_proto)
  → 檢查 indices[1][hash2] 是否存在
  → 如果不存在，提前返回

階段 3：用所有欄位
  → 計算 hash3 = hash(flow->all_fields)
  → 在 subtable->rules[hash3] 中查找匹配規則
```

**優勢**：
- **快速過濾**：大部分不匹配的封包在階段 1-2 就被過濾
- **減少衝突**：每階段只用少數欄位，哈希衝突少
- **階段數量**：通常 2-3 個，根據規則數量動態調整

---

### 4. 並發優化

**RCU（Read-Copy-Update）**：
```c
// 讀者無鎖
const struct cls_match *match = ovsrcu_get(&rule->next);

// 寫者更新
ovsrcu_set(&rule->next, new_match);
ovsrcu_postpone(free, old_match);  // 延遲釋放
```

**CMAP（並發哈希表）**：
```c
// 支持並發讀寫
CMAP_FOR_EACH_WITH_HASH (rule, cmap_node, hash, &subtable->rules) {
    // ...
}
```

---

## 與 DT 的對比

### 架構對比

| 特性 | TSS | DT |
|------|-----|-----|
| **分組方式** | 按 mask 分組（subtables） | 按欄位值分組（決策樹節點） |
| **欄位選擇** | 自動（所有欄位） | 人工指定候選欄位 |
| **查找方式** | 遍歷 subtables + 哈希查找 | 樹遍歷（二分查找） |
| **複雜度** | O(n_subtables) × O(1) 哈希 | O(log n_rules) 理想情況 |
| **內存** | O(n_rules) + O(n_subtables) | O(n_rules) + O(tree_nodes) |

---

### 欄位利用對比

#### TSS：所有欄位都可用

```c
// 自動支持所有 173 個欄位
Rule1: in_port=1, vlan_vid=100, ipv6_src=2001:db8::1/128, 
       ct_state=+trk, tun_id=42, nsh_spi=100, reg0=0x123
       → 自動創建 subtable

✅ 優點：完全透明，無需配置
✅ 靈活：支持任意欄位組合
⚠️ 開銷：每種 mask 都需要一個 subtable
```

#### DT：只用 9 個候選欄位

```c
// lib/dt-classifier.c:613-625
static const enum mf_field_id candidate_fields[] = {
    MFF_IN_PORT, MFF_ETH_TYPE,
    MFF_IPV4_SRC, MFF_IPV4_DST, MFF_IP_PROTO,
    MFF_TCP_SRC, MFF_TCP_DST, MFF_UDP_SRC, MFF_UDP_DST,
};

⚠️ 限制：不支持 IPv6, VLAN, tunnel, CT, registers, ...
✅ 高效：只考慮核心欄位，樹更簡單
✅ 可控：明確的欄位選擇策略
```

---

### 查找性能對比

#### 場景 1：少量 subtables（< 10 個）

**範例規則集**：
```
所有規則都是 ip_dst/32 匹配
→ 只有 1 個 subtable
```

**TSS 性能**：
- 遍歷 subtables: O(1)
- 哈希查找: O(1)
- **總計**: O(1) ✅ **極快**

**DT 性能**：
- 樹遍歷: O(log n)
- **總計**: O(log n) ⚠️ 較慢

**結論**：TSS 勝出

---

#### 場景 2：大量 subtables（> 100 個）

**範例規則集**：
```
每條規則都有不同的 mask 組合
Rule1: ip_dst/32
Rule2: ip_dst/24, tp_dst
Rule3: ip_src/16, ip_dst/16
...
→ 100 個 subtables
```

**TSS 性能**：
- 遍歷 subtables: O(100)（即使用 trie 優化）
- **總計**: O(100) ⚠️ **較慢**

**DT 性能**：
- 樹遍歷: O(log n)
- **總計**: O(log n) ✅ **較快**

**結論**：DT 勝出

---

#### 場景 3：複雜欄位組合

**範例規則集**：
```
Rule1: vlan_vid=100, ipv6_src=2001:db8::1/64, ct_state=+trk
Rule2: tun_id=42, mpls_label=100, reg0=0x123
```

**TSS 性能**：
- ✅ 完全支持
- 正常查找流程

**DT 性能**：
- ❌ 不支持（candidate_fields 沒有這些欄位）
- 無法分類

**結論**：TSS 勝出（DT 根本不支持）

---

### 增量更新對比

#### TSS：完全支持

```c
// 插入規則
classifier_replace(cls, rule, version, conjs, n_conjs);
  → 查找或創建 subtable
  → 插入規則到 subtable->rules
  → 更新 trie
  → O(1) 時間

// 刪除規則
classifier_remove(cls, rule);
  → 從 subtable->rules 中刪除
  → 更新 trie
  → 如果 subtable 為空，刪除 subtable
  → O(1) 時間

✅ 增量更新非常高效
✅ 不需要重建數據結構
```

#### DT：理論支持，實際未實現

```c
// 當前實現：
// - dt_insert_rule(): 只添加到葉節點，不分裂
// - dt_remove_rule(): 只標記刪除，不合併
// - 沒有樹重平衡機制

⚠️ 理論上支持，但需要實現：
  1. 葉節點分裂（已有框架）
  2. 樹重平衡
  3. 節點合併
```

---

## 總結

### TSS 的欄位利用策略

1. **完全透明**：所有 173 個欄位都可以用於匹配
2. **自動分組**：根據 mask 自動創建 subtables
3. **靈活組合**：支持任意欄位組合
4. **多重優化**：
   - Trie：IP 前綴快速過濾
   - Ports Trie：端口快速過濾
   - Staged Lookup：多階段哈希減少衝突
   - 優先級排序：高優先級 subtables 優先檢查

### TSS vs DT

| 維度 | TSS 優勢 | DT 優勢 |
|------|---------|---------|
| **欄位支持** | ✅ 所有 173 個欄位 | ⚠️ 只有 9 個 |
| **靈活性** | ✅ 任意組合 | ⚠️ 有限 |
| **增量更新** | ✅ 完全支持 | ⚠️ 部分支持 |
| **少 subtables** | ✅ 極快 | ⚠️ 較慢 |
| **多 subtables** | ⚠️ 較慢 | ✅ 更快 |
| **內存開銷** | ⚠️ subtables 開銷 | ✅ 較小 |

### 適用場景

**TSS 最適合**：
- ✅ 複雜匹配條件（多種欄位組合）
- ✅ 頻繁更新（高動態場景）
- ✅ mask 模式少（< 20 個 subtables）
- ✅ 需要支持所有欄位

**DT 最適合**：
- ✅ 簡單匹配條件（只用核心欄位）
- ✅ 靜態規則集（更新不頻繁）
- ✅ mask 模式多（> 100 個 subtables）
- ✅ 查找性能關鍵

### 關鍵代碼位置

```
TSS 核心實現：
├── lib/classifier.c           (主邏輯)
│   ├── classifier_lookup__()  (查找入口)
│   ├── find_match_wc()        (subtable 內查找)
│   ├── classifier_replace()   (插入規則)
│   └── classifier_remove()    (刪除規則)
├── lib/classifier-private.h   (數據結構)
│   ├── struct cls_subtable   (子表)
│   └── struct cls_match       (規則)
├── lib/flow.c                 (匹配邏輯)
│   └── miniflow_and_mask_matches_flow()
└── lib/ccmap.c                (並發哈希表)
```

---

## 延伸閱讀

1. **官方文檔**：
   - `Documentation/topics/datapath.rst` - Datapath 設計
   - `lib/classifier.c` 開頭的註釋 - TSS 演算法說明

2. **論文**：
   - "The Open vSwitch Megaflow Cache" (USENIX NSDI 2014)
   - "Tuple Space Explosion" 問題分析

3. **相關文檔**：
   - `OVS_PACKET_FIELDS_FOR_CLASSIFICATION.md` - 欄位列表
   - `DT_ALGORITHM_EXPLAINED.md` - DT 演算法對比
   - `MEGAFLOW_UNIQUENESS_EXPLAINED.md` - Megaflow 機制
