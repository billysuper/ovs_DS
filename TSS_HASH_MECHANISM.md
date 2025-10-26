# TSS Hash 計算機制詳解

## 目錄
1. [核心問題](#核心問題)
2. [答案總結](#答案總結)
3. [Hash 計算原理](#hash-計算原理)
4. [代碼實現](#代碼實現)
5. [實例演示](#實例演示)
6. [性能影響](#性能影響)
7. [與其他方案對比](#與其他方案對比)

---

## 核心問題

**問題**：TSS 會將所有欄位納入 hash code 計算嗎？

---

## 答案總結

### ❌ **不會！TSS 只計算 mask 中標記為非 wildcard 的欄位**

```
關鍵規則：
┌────────────────────────────────────────────────────────┐
│  只有 mask 中為 1 的位元（非 wildcard）才會被納入      │
│  hash 計算，wildcard 的欄位完全被忽略                  │
│                                                        │
│  公式：                                                │
│    hash = hash_function(flow_value & mask)            │
│                                                        │
│  範例：                                                │
│    mask: {ip_dst = 0xFFFFFF00, tp_dst = 0xFFFF}       │
│    flow: {ip_dst = 10.0.1.5, tp_dst = 80, ...}        │
│                                                        │
│    計算 hash 時：                                      │
│    ✅ ip_dst: 參與（mask ≠ 0）                        │
│    ✅ tp_dst: 參與（mask ≠ 0）                        │
│    ❌ in_port, eth_type, ...: 不參與（mask = 0）      │
└────────────────────────────────────────────────────────┘
```

### 這樣設計的原因

1. **保證相同 mask 的規則可以在同一 subtable 中查找**
2. **減少 hash 衝突**（只用有效欄位）
3. **提高性能**（不計算無關欄位）

---

## Hash 計算原理

### 基本概念

```
TSS 使用三層 hash：

Layer 1: Subtable Hash
  → 用 mask 本身計算 hash
  → 用於查找正確的 subtable

Layer 2: Staged Lookup Hash
  → 在 subtable 內部，分階段計算 hash
  → 用於快速過濾規則

Layer 3: Final Hash
  → 計算所有非 wildcard 欄位的 hash
  → 用於最終查找規則
```

---

## 代碼實現

### 1. Subtable Hash（查找 Subtable）

**代碼位置**：`lib/classifier.c:1477, 1539`

```c
// 根據 mask 計算 hash，查找對應的 subtable
CMAP_FOR_EACH_WITH_HASH (subtable, cmap_node, 
                         minimask_hash(mask, 0),
                         &cls->subtables_map) {
    // ...
}
```

**作用**：
- 根據規則的 **mask** 找到對應的 subtable
- 相同 mask 的規則會被放在同一個 subtable

---

### 2. Flow Hash in Minimask（核心函數）

**代碼位置**：`lib/classifier-private.h:215-233`

```c
/* Returns a hash value for the bits of 'flow' where there are 1-bits in
 * 'mask', given 'basis'.
 *
 * 關鍵點：只計算 mask 中為 1 的位元
 */
static inline uint32_t
flow_hash_in_minimask(const struct flow *flow, 
                      const struct minimask *mask,
                      uint32_t basis)
{
    const uint64_t *mask_values = miniflow_get_values(&mask->masks);
    const uint64_t *flow_u64 = (const uint64_t *)flow;
    const uint64_t *p = mask_values;
    uint32_t hash = basis;
    map_t map;

    // 遍歷 mask 中所有非零的 64-bit 單元
    FLOWMAP_FOR_EACH_MAP (map, mask->masks.map) {
        size_t idx;

        // 對每個單元，計算 (flow_value & mask_value) 的 hash
        MAP_FOR_EACH_INDEX (idx, map) {
            hash = hash_add64(hash, flow_u64[idx] & *p++);
            //                      ^^^^^^^^^^^^^^^^^^^^^^
            //                      關鍵操作：flow AND mask
        }
        flow_u64 += MAP_T_BITS;
    }

    // 完成 hash 計算
    return hash_finish(hash, (p - mask_values) * 8);
}
```

**核心邏輯**：
```c
hash = hash_add64(hash, flow_u64[idx] & *p++);
                        ^^^^^^^^^^^^^^^^^^
                        flow_value & mask_value

如果 mask_value = 0，則 (flow_value & 0) = 0
→ 該欄位不影響 hash 值
```

---

### 3. Miniflow Hash in Minimask（壓縮版本）

**代碼位置**：`lib/classifier-private.h:243-255`

```c
/* 與 flow_hash_in_minimask() 相同，但參數是 miniflow */
static inline uint32_t
miniflow_hash_in_minimask(const struct miniflow *flow,
                          const struct minimask *mask, 
                          uint32_t basis)
{
    const uint64_t *mask_values = miniflow_get_values(&mask->masks);
    const uint64_t *p = mask_values;
    uint32_t hash = basis;
    uint64_t value;

    // 只遍歷 mask 中指定的欄位
    MINIFLOW_FOR_EACH_IN_FLOWMAP(value, flow, mask->masks.map) {
        hash = hash_add64(hash, value & *p++);
        //                      ^^^^^^^^^^^
        //                      value 已經是從 miniflow 中提取的
        //                      只包含非 wildcard 的欄位
    }

    return hash_finish(hash, (p - mask_values) * 8);
}
```

**優化點**：
- `miniflow` 本身只存儲非 wildcard 的欄位
- `mask->masks.map` 指示哪些欄位需要參與計算
- 雙重壓縮：miniflow + minimask

---

### 4. Staged Lookup Hash（多階段 Hash）

**代碼位置**：`lib/classifier-private.h:272-299`

```c
/* 計算指定範圍內的 hash
 * range: 指定哪些欄位參與本次計算（階段）
 * offset: 當前處理到 mask 的哪個位置
 * basis: hash 基礎值，允許多階段累積
 */
static inline uint32_t
flow_hash_in_minimask_range(const struct flow *flow,
                            const struct minimask *mask,
                            const struct flowmap range,  // 本階段的欄位範圍
                            unsigned int *offset,
                            uint32_t *basis)
{
    const uint64_t *mask_values = miniflow_get_values(&mask->masks);
    const uint64_t *flow_u64 = (const uint64_t *)flow;
    const uint64_t *p = mask_values + *offset;  // 從上次結束的地方繼續
    uint32_t hash = *basis;  // 使用上次的 hash 作為基礎
    map_t map;

    // 只遍歷 range 指定的欄位
    FLOWMAP_FOR_EACH_MAP (map, range) {
        size_t idx;

        MAP_FOR_EACH_INDEX (idx, map) {
            hash = hash_add64(hash, flow_u64[idx] & *p++);
        }
        flow_u64 += MAP_T_BITS;
    }

    *basis = hash;   // 更新 basis，下個階段繼續使用
    *offset = p - mask_values;  // 更新 offset
    return hash_finish(hash, *offset * 8);
}
```

**使用場景**：
```c
// 在 find_match_wc() 中的使用
// lib/classifier.c:1750-1766

// 階段 1: 只計算前幾個欄位的 hash
hash = flow_hash_in_minimask_range(flow, &subtable->mask,
                                   subtable->index_maps[0],  // 階段 0 的欄位
                                   &mask_offset, &basis);

// 檢查階段 1 的索引
if (!ccmap_find(&subtable->indices[0], hash)) {
    return NULL;  // 提前返回，無需計算後續階段
}

// 階段 2: 繼續計算下一組欄位
hash = flow_hash_in_minimask_range(flow, &subtable->mask,
                                   subtable->index_maps[1],  // 階段 1 的欄位
                                   &mask_offset, &basis);
// ...
```

---

## 實例演示

### 範例 1：簡單規則

**規則定義**：
```c
Rule: ip_dst=10.0.1.5/32, tp_dst=80, priority=100
```

**Mask 提取**：
```c
mask = {
    ip_dst:  0xFFFFFFFF,  // /32 完整匹配
    tp_dst:  0xFFFF,      // 端口完整匹配
    其他欄位: 0x00...00   // wildcard
}
```

**Hash 計算**：
```c
// struct flow 包含所有 173 個欄位
struct flow packet = {
    in_port: 1,
    eth_type: 0x0800,
    ip_dst: 10.0.1.5,      // 0x0A000105
    tp_dst: 80,            // 0x0050
    tp_src: 12345,
    // ... 其他欄位
};

// 計算 hash
hash = flow_hash_in_minimask(&packet, &mask, 0);

// 實際計算過程（偽代碼）：
hash = basis;  // 0
hash = hash_add64(hash, 0x0A000105 & 0xFFFFFFFF);  // ip_dst
       ✅ ip_dst 參與計算（mask ≠ 0）
hash = hash_add64(hash, 0x0050 & 0xFFFF);          // tp_dst
       ✅ tp_dst 參與計算（mask ≠ 0）

// in_port, eth_type, tp_src, ... 都不參與計算（mask = 0）
❌ hash ≠ hash_add64(hash, 1);              // in_port 被忽略
❌ hash ≠ hash_add64(hash, 0x0800);         // eth_type 被忽略
❌ hash ≠ hash_add64(hash, 12345);          // tp_src 被忽略

return hash_finish(hash, 16);  // 16 = 2 個欄位 × 8 bytes
```

**結果**：
- ✅ 只有 `ip_dst` 和 `tp_dst` 參與 hash 計算
- ❌ 其他 171 個欄位完全被忽略

---

### 範例 2：複雜規則

**規則定義**：
```c
Rule: in_port=1, vlan_vid=100, ipv6_src=2001:db8::/64, 
      ct_state=+trk+new, priority=100
```

**Mask 提取**：
```c
mask = {
    in_port:   0xFFFF,              // 2 bytes
    vlan_vid:  0x0FFF,              // 12 bits
    ipv6_src:  0xFFFFFFFF FFFFFFFF 0000000000000000,  // /64
    ct_state:  0xFF,                // 8 bits
    其他欄位:  0x00...00            // wildcard
}
```

**Hash 計算**：
```c
struct flow packet = {
    in_port: 1,                    // ✅ 參與
    vlan_vid: 100,                 // ✅ 參與
    ipv6_src: 2001:db8::1234,      // ✅ 前 64 bits 參與
    ct_state: 0x23,                // ✅ 參與
    eth_type: 0x86dd,              // ❌ 不參與（mask = 0）
    tp_dst: 443,                   // ❌ 不參與（mask = 0）
    // ... 其他欄位              // ❌ 不參與
};

hash = flow_hash_in_minimask(&packet, &mask, 0);

// 實際計算（偽代碼）：
hash = hash_add64(hash, 1 & 0xFFFF);                    // in_port
hash = hash_add64(hash, 100 & 0x0FFF);                  // vlan_vid
hash = hash_add64(hash, 0x20010db8... & 0xFFFFFFFF...); // ipv6_src 前 64 bits
hash = hash_add64(hash, 0x23 & 0xFF);                   // ct_state

// eth_type, tp_dst, ... 不參與計算
return hash_finish(hash, N * 8);  // N = 參與計算的 64-bit 單元數
```

**結果**：
- ✅ 4 個欄位參與計算
- ❌ 169 個欄位被忽略

---

### 範例 3：Catchall 規則

**規則定義**：
```c
Rule: priority=1  (沒有任何匹配條件，接受所有封包)
```

**Mask 提取**：
```c
mask = {
    所有欄位: 0x00...00  // 全部 wildcard
}
```

**Hash 計算**：
```c
hash = flow_hash_in_minimask(&packet, &mask, 0);

// 實際計算：
// FLOWMAP_FOR_EACH_MAP 會遍歷 mask->masks.map
// 但 mask->masks.map 是空的（所有位都是 0）
// 所以循環體不會執行

hash = basis;  // 0
return hash_finish(0, 0);  // 沒有任何欄位參與計算
```

**結果**：
- ❌ **沒有任何欄位參與計算**
- Hash 值是常數（只依賴 basis）
- 所有封包的 hash 都相同

---

## 性能影響

### 1. Hash 計算開銷

```
開銷 = O(參與計算的欄位數量)

範例：
┌────────────────────────────────────────────────┐
│ Rule A: ip_dst/32, tp_dst                      │
│   → 2 個欄位參與計算                           │
│   → 開銷：O(2)                                 │
├────────────────────────────────────────────────┤
│ Rule B: in_port, eth_type, ip_dst/32,          │
│         tp_dst, vlan_vid                       │
│   → 5 個欄位參與計算                           │
│   → 開銷：O(5)                                 │
├────────────────────────────────────────────────┤
│ Catchall: (無條件)                             │
│   → 0 個欄位參與計算                           │
│   → 開銷：O(1) 常數                            │
└────────────────────────────────────────────────┘
```

### 2. Hash 衝突率

**只計算非 wildcard 欄位的好處**：

```
假設場景：
  1000 條規則，都匹配 ip_dst/32
  每條規則的 ip_dst 都不同

如果計算所有欄位的 hash：
  → 其他欄位（如 tp_src）可能導致不必要的衝突
  → 即使 ip_dst 不同，其他欄位相同也會增加衝突

只計算 ip_dst：
  → 完美分佈（每條規則 hash 不同）
  → 最小化衝突
```

### 3. 內存訪問

**Miniflow 優化**：
```c
// 原始 struct flow: 240 bytes
struct flow {
    /* 173 個欄位，大部分是 0 */
};

// Miniflow: 只存儲非零欄位
struct miniflow {
    struct flowmap map;      // 指示哪些欄位非零
    uint64_t values[];       // 緊湊存儲
};

優勢：
  ✅ 減少內存訪問（只訪問有效欄位）
  ✅ 更好的緩存局部性
  ✅ 更快的 hash 計算
```

---

## 與其他方案對比

### 1. 計算所有欄位的 Hash（簡單但低效）

```c
// 假設的實現
uint32_t flow_hash_all_fields(const struct flow *flow, uint32_t basis)
{
    uint32_t hash = basis;
    
    // 計算所有 240 bytes
    for (size_t i = 0; i < FLOW_U64S; i++) {
        hash = hash_add64(hash, ((uint64_t *)flow)[i]);
    }
    
    return hash_finish(hash, FLOW_U64S * 8);
}
```

**問題**：
```
❌ 浪費計算：wildcard 欄位不影響匹配，但參與 hash
❌ 增加衝突：不同規則可能在無關欄位上衝突
❌ 性能差：必須訪問所有 240 bytes

範例：
  Rule1: ip_dst=10.0.1.5, (tp_dst=wildcard)
  Rule2: ip_dst=10.0.1.5, (tp_dst=wildcard)
  
  如果計算所有欄位：
    Packet1: ip_dst=10.0.1.5, tp_dst=80
    Packet2: ip_dst=10.0.1.5, tp_dst=443
    → hash(Packet1) ≠ hash(Packet2)
    → 可能放在不同的 bucket，需要檢查兩次
  
  只計算 ip_dst：
    hash(Packet1) = hash(Packet2)
    → 放在同一個 bucket，一次檢查
```

---

### 2. 5-Tuple Hash（常見但有限）

**代碼位置**：`lib/flow.c:2300-2342`

```c
uint32_t
miniflow_hash_5tuple(const struct miniflow *flow, uint32_t basis)
{
    uint32_t hash = basis;

    // 只計算 5 個欄位：
    // - ip_src, ip_dst (or ipv6_src, ipv6_dst)
    // - nw_proto
    // - tp_src, tp_dst
    
    if (dl_type == ETH_TYPE_IP) {
        hash = hash_add(hash, MINIFLOW_GET_U32(flow, nw_src));
        hash = hash_add(hash, MINIFLOW_GET_U32(flow, nw_dst));
    } else if (dl_type == ETH_TYPE_IPV6) {
        // IPv6 src/dst
    }
    
    hash = hash_add(hash, nw_proto);
    hash = hash_add(hash, miniflow_get_ports(flow));
    
    return hash_finish(hash, 42);
}
```

**用途**：
- Megaflow cache（快速查找）
- 負載均衡（ECMP hash）

**限制**：
```
❌ 只支持 L3/L4 匹配
❌ 不支持 L2 匹配（VLAN, MAC）
❌ 不支持元數據（in_port, metadata）
❌ 不支持高級特性（CT, tunnel, NSH）
```

---

### 3. TSS 的 Flow Hash in Minimask（完美平衡）

**優勢**：
```
✅ 靈活：支持所有 173 個欄位的任意組合
✅ 高效：只計算非 wildcard 欄位
✅ 最小衝突：針對實際匹配條件優化
✅ 可擴展：新欄位無需修改 hash 函數
```

**對比**：

| 方案 | 支持欄位 | 計算開銷 | 衝突率 | 靈活性 |
|------|---------|---------|--------|--------|
| **所有欄位** | 173 個 | O(173) | 高 | ❌ |
| **5-Tuple** | 5 個 | O(5) | 中 | ❌ |
| **TSS (Minimask)** | 動態 | O(N) | 最低 | ✅ |

其中 N = 實際匹配的欄位數量（通常 1-10 個）

---

## 總結

### 關鍵要點

1. **TSS 不會計算所有欄位的 hash**
   - ✅ 只計算 mask 中非 wildcard 的欄位
   - ❌ Wildcard 欄位完全被忽略

2. **核心公式**
   ```c
   hash = hash_function(flow_value & mask_value)
   
   如果 mask_value = 0:
     → flow_value & 0 = 0
     → 該欄位不影響 hash
   ```

3. **三種 Hash 函數**
   - `flow_hash_in_minimask()`: 完整版本
   - `miniflow_hash_in_minimask()`: 壓縮版本
   - `flow_hash_in_minimask_range()`: 階段版本

4. **性能優勢**
   - 計算開銷：O(實際匹配的欄位數)
   - 通常只需計算 2-10 個欄位
   - 遠少於 173 個總欄位

5. **設計優點**
   - ✅ 靈活：支持任意欄位組合
   - ✅ 高效：最小化計算和衝突
   - ✅ 可擴展：新欄位自動支持

### 實際影響

```
典型規則：ip_dst=10.0.1.5/32, tp_dst=80

參與 hash 的欄位：2 個
  ✅ ip_dst  (4 bytes)
  ✅ tp_dst  (2 bytes)

不參與 hash 的欄位：171 個
  ❌ in_port, eth_type, eth_src, eth_dst, vlan_vid, 
     ip_src, tp_src, ct_state, tun_id, registers, ...
     (共 234 bytes)

計算效率：
  實際計算：6 bytes
  總大小：240 bytes
  效率：6/240 = 2.5% 的數據量
  速度提升：約 40 倍（理論值）
```

### 與 DT 的對比

| 維度 | TSS Hash | DT 決策樹 |
|------|----------|-----------|
| **欄位選擇** | 動態（根據 mask） | 靜態（候選欄位） |
| **計算方式** | Hash（O(N)） | 比較（O(log n)） |
| **靈活性** | 完全透明 | 需要配置 |
| **開銷** | 低（只計算有效欄位） | 低（樹遍歷） |

---

## 代碼位置總結

```
Hash 計算核心實現：
├── lib/classifier-private.h
│   ├── flow_hash_in_minimask()          (line 215-233)
│   ├── miniflow_hash_in_minimask()      (line 243-255)
│   └── flow_hash_in_minimask_range()    (line 272-299)
│
├── lib/classifier.c
│   ├── find_match_wc()                  (line 1720-1850)
│   │   └── 使用 flow_hash_in_minimask_range() 進行階段查找
│   └── classifier_lookup__()            (line 958-1098)
│       └── 主查找循環
│
└── lib/flow.c
    ├── miniflow_hash_5tuple()           (line 2300-2342)
    └── flow_hash_5tuple()               (line 2350-2390)
        └── 特殊用途的 5-tuple hash
```

---

## 延伸閱讀

1. **相關文檔**：
   - `TSS_CLASSIFICATION_MECHANISM.md` - TSS 分類機制
   - `OVS_PACKET_FIELDS_FOR_CLASSIFICATION.md` - 欄位列表

2. **源代碼**：
   - `lib/classifier-private.h` - Hash 函數實現
   - `lib/flow.h` - Miniflow/Minimask 數據結構
   - `lib/flow.c` - Flow 處理邏輯

3. **性能分析**：
   - Staged lookup 優化
   - Miniflow 內存優化
   - Hash 衝突處理
