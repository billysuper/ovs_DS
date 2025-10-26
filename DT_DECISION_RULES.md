# Decision Tree 決策規則制定機制

**創建日期**: 2025-10-17  
**核心問題**: Decision Tree 如何決定在每個節點測試什麼？如何選擇分割點？

---

## 📖 目錄

1. [核心問題](#核心問題)
2. [決策流程總覽](#決策流程總覽)
3. [欄位選擇算法](#欄位選擇算法)
4. [分割值選擇](#分割值選擇)
5. [測試類型決定](#測試類型決定)
6. [實際範例](#實際範例)
7. [優化策略](#優化策略)

---

## 🎯 核心問題

### 決策樹需要回答的三個問題

在構建 Decision Tree 的每個內部節點時，需要決定：

```
1. 測試哪個欄位？
   ❓ in_port? src_ip? dst_port? ...
   
2. 如何測試？
   ❓ 精確匹配？前綴匹配？範圍匹配？
   
3. 測試什麼值？
   ❓ == 10.0.0.1? in 192.168.0.0/16? ...
```

### 為什麼這很重要？

```
好的決策 → 平衡的樹 → O(log n) 查找 ✅
壞的決策 → 不平衡的樹 → O(n) 查找 ❌

範例：
規則集: [R1, R2, R3, R4, R5, R6, R7, R8]

好的決策（平衡）:
              [Node: 測試 A]
              /            \
       [R1,R2,R3,R4]    [R5,R6,R7,R8]
            /  \            /  \
         [R1,R2][R3,R4]  [R5,R6][R7,R8]

查找深度: 3

壞的決策（不平衡）:
    [Node: 測試 B]
    /            \
  [R1]     [Node: 測試 C]
           /            \
         [R2]     [Node: 測試 D]
                  /            \
                [R3]          [...]

查找深度: 7+
```

---

## 🔄 決策流程總覽

### 樹構建的完整流程

```
輸入: 規則集 [R1, R2, ..., Rn]
        │
        ▼
┌─────────────────────┐
│ 1. 檢查終止條件     │
│    - 規則數 ≤ 閾值？│
│    - 無法繼續分割？ │
└──────┬──────────────┘
       │ 不終止
       ▼
┌─────────────────────┐
│ 2. 選擇分割欄位     │
│    ❓ 測試哪個欄位？│
└──────┬──────────────┘
       │
       ▼
┌─────────────────────┐
│ 3. 決定測試類型     │
│    ❓ 如何測試？    │
└──────┬──────────────┘
       │
       ▼
┌─────────────────────┐
│ 4. 選擇分割值       │
│    ❓ 測試什麼值？  │
└──────┬──────────────┘
       │
       ▼
┌─────────────────────┐
│ 5. 分割規則集       │
│    - 匹配的 → 右邊  │
│    - 不匹配 → 左邊  │
└──────┬──────────────┘
       │
       ▼
┌─────────────────────┐
│ 6. 遞歸構建子樹     │
│    - 左子樹(規則集1)│
│    - 右子樹(規則集2)│
└─────────────────────┘
```

---

## 🎲 欄位選擇算法

### 當前實現：使用頻率法

**位置**: `lib/dt-classifier.c:602-658`

#### 算法邏輯

```c
/* 步驟 1: 定義候選欄位 */
候選欄位列表 = [
    MFF_IN_PORT,      // 入口埠
    MFF_ETH_TYPE,     // 以太網類型
    MFF_IPV4_SRC,     // 源 IP
    MFF_IPV4_DST,     // 目標 IP
    MFF_IP_PROTO,     // IP 協議
    MFF_TCP_SRC,      // TCP 源埠
    MFF_TCP_DST,      // TCP 目標埠
    MFF_UDP_SRC,      // UDP 源埠
    MFF_UDP_DST,      // UDP 目標埠
];

/* 步驟 2: 統計每個欄位的使用次數 */
for each 候選欄位 in 候選欄位列表 {
    計數 = 0;
    
    for each 規則 in 規則集 {
        if (規則匹配這個欄位 && 欄位不是全通配) {
            計數++;
        }
    }
    
    欄位計數[欄位] = 計數;
}

/* 步驟 3: 選擇使用次數最多的欄位 */
最佳欄位 = 欄位計數最大的那個;
return 最佳欄位;
```

#### 實際代碼

```c
static const struct mf_field *
dt_select_split_field(struct rculist *rules, size_t n_rules)
{
    // 候選欄位
    static const enum mf_field_id candidate_fields[] = {
        MFF_IN_PORT,
        MFF_ETH_TYPE,
        MFF_IPV4_SRC,
        MFF_IPV4_DST,
        MFF_IP_PROTO,
        MFF_TCP_SRC,
        MFF_TCP_DST,
        MFF_UDP_SRC,
        MFF_UDP_DST,
    };
    
    // 統計使用次數
    int field_counts[ARRAY_SIZE(candidate_fields)] = {0};
    
    for (size_t i = 0; i < ARRAY_SIZE(candidate_fields); i++) {
        const struct mf_field *field = mf_from_id(candidate_fields[i]);
        const struct cls_rule *rule;
        
        RCULIST_FOR_EACH (rule, node, rules) {
            union mf_value value, mask;
            mf_get(&field->id, &rule->match, &value, &mask);
            
            // 如果 mask 不全是 0（即規則關心這個欄位）
            if (!is_all_zeros(&mask, field->n_bytes)) {
                field_counts[i]++;  // 計數++
            }
        }
    }
    
    // 選擇計數最高的欄位
    int best_idx = 0;
    int best_count = field_counts[0];
    
    for (size_t i = 1; i < ARRAY_SIZE(candidate_fields); i++) {
        if (field_counts[i] > best_count) {
            best_count = field_counts[i];
            best_idx = i;
        }
    }
    
    return mf_from_id(candidate_fields[best_idx]);
}
```

### 範例：欄位選擇過程

```
規則集:
  R1: match src_ip=10.0.0.1, dst_port=80,    priority=100
  R2: match src_ip=10.0.0.2, dst_port=80,    priority=90
  R3: match src_ip=10.0.0.3, dst_port=443,   priority=80
  R4: match dst_ip=192.168.1.1, dst_port=22, priority=70
  R5: match (catchall),                      priority=50

步驟 1: 統計各欄位使用次數
┌──────────┬────────────────────────┬───────┐
│   欄位   │     使用的規則         │ 計數  │
├──────────┼────────────────────────┼───────┤
│ src_ip   │ R1, R2, R3             │  3    │ ← 最高！
│ dst_ip   │ R4                     │  1    │
│ dst_port │ R1, R2, R3, R4         │  4    │ ← 最高！
│ catchall │ R5                     │  0    │
└──────────┴────────────────────────┴───────┘

步驟 2: 選擇計數最高的欄位
結果: dst_port (計數=4)

為什麼選 dst_port？
  → 最多規則關心這個欄位
  → 分割效果可能最好
  → 可以把規則集分成有意義的子集
```

### 進階：資訊增益法（未實現）

#### 基本概念

```
資訊熵 (Entropy):
  H(S) = -Σ p(i) * log2(p(i))
  
  測量規則集的「混亂程度」
  
  範例:
    規則集 [R1, R2, R3, R4]，優先級都不同
    → 熵高（很混亂）
    
    規則集 [R1, R1, R1, R1]，優先級都相同
    → 熵低（很有序）

資訊增益 (Information Gain):
  IG(S, A) = H(S) - Σ |Sv|/|S| * H(Sv)
  
  測量「選擇欄位 A 分割後，熵減少了多少」
  
  增益越大 → 分割效果越好 → 選這個欄位！
```

#### 算法流程

```
for each 候選欄位 {
    計算當前規則集的熵 H(S);
    
    用這個欄位分割規則集 → 左子集 + 右子集;
    
    計算分割後的加權平均熵:
        H_after = (|左| / |總|) * H(左) + (|右| / |總|) * H(右);
    
    資訊增益 = H(S) - H_after;
}

選擇資訊增益最大的欄位;
```

#### 範例：資訊增益計算

```
規則集 S = [R1(p=100), R2(p=90), R3(p=80), R4(p=70)]

初始熵:
  H(S) = -Σ (1/4) * log2(1/4) = 2.0 (很混亂)

選項 A: 用 src_ip 分割
  左子集: [R1, R2]  → H = 1.0
  右子集: [R3, R4]  → H = 1.0
  H_after = (2/4)*1.0 + (2/4)*1.0 = 1.0
  IG = 2.0 - 1.0 = 1.0 ✓

選項 B: 用 dst_port 分割
  左子集: [R1, R2, R3]  → H = 1.58
  右子集: [R4]          → H = 0
  H_after = (3/4)*1.58 + (1/4)*0 = 1.19
  IG = 2.0 - 1.19 = 0.81

選項 A 的資訊增益更高 → 選擇 src_ip ✓
```

---

## 🎯 分割值選擇

### 當前實現：中位數法（簡化版）

**位置**: `lib/dt-classifier.c:666-691`

#### 算法邏輯

```c
/* 步驟 1: 收集所有規則在該欄位的值 */
值列表 = [];

for each 規則 in 規則集 {
    從規則中提取該欄位的值;
    值列表.add(值);
}

/* 步驟 2: 選擇中位數作為分割點 */
排序(值列表);
分割值 = 值列表[中間位置];

return 分割值;
```

#### 當前代碼（簡化版）

```c
static bool
dt_find_split_value(const struct mf_field *field, struct rculist *rules,
                    enum dt_test_type *test_type, ovs_be32 *split_value,
                    unsigned int *plen)
{
    *test_type = DT_TEST_EXACT;  // 默認：精確匹配
    
    // 收集值
    ovs_be32 values[128];
    size_t n_values = 0;
    
    RCULIST_FOR_EACH (rule, node, rules) {
        if (n_values >= 128) break;
        
        // ⚠️ 當前使用 dummy 值（待完善）
        values[n_values++] = htonl(n_values);
    }
    
    // 選擇中位數
    *split_value = values[n_values / 2];
    
    return true;
}
```

#### ⚠️ 當前限制

```
問題：使用 dummy 值而不是實際規則的值

當前:
  values[0] = htonl(0)   // dummy
  values[1] = htonl(1)   // dummy
  values[2] = htonl(2)   // dummy

應該是:
  values[0] = 從 R1 提取的 src_ip = 10.0.0.1
  values[1] = 從 R2 提取的 src_ip = 10.0.0.2
  values[2] = 從 R3 提取的 src_ip = 10.0.0.3
```

### 完整版：實際值提取

```c
static bool
dt_find_split_value(const struct mf_field *field, struct rculist *rules,
                    enum dt_test_type *test_type, ovs_be32 *split_value,
                    unsigned int *plen)
{
    ovs_be32 values[128];
    size_t n_values = 0;
    
    // 從規則中提取實際值
    const struct cls_rule *rule;
    RCULIST_FOR_EACH (rule, node, rules) {
        if (n_values >= 128) break;
        
        union mf_value value, mask;
        mf_get(&field->id, &rule->match, &value, &mask);
        
        // 只收集非通配的值
        if (!is_all_zeros(&mask, field->n_bytes)) {
            values[n_values++] = value.be32;
        }
    }
    
    if (n_values == 0) {
        return false;  // 沒有可用的值
    }
    
    // 排序值
    qsort(values, n_values, sizeof(ovs_be32), compare_be32);
    
    // 選擇中位數
    *split_value = values[n_values / 2];
    
    // 決定測試類型
    if (has_common_prefix(values, n_values)) {
        *test_type = DT_TEST_PREFIX;
        *plen = calculate_prefix_length(values, n_values);
    } else {
        *test_type = DT_TEST_EXACT;
    }
    
    return true;
}
```

### 範例：分割值選擇

```
規則集（已選擇欄位：src_ip）:
  R1: src_ip = 10.0.0.1
  R2: src_ip = 10.0.0.2
  R3: src_ip = 10.0.0.5
  R4: src_ip = 192.168.1.1
  R5: src_ip = 192.168.1.2

步驟 1: 收集值
  values = [10.0.0.1, 10.0.0.2, 10.0.0.5, 192.168.1.1, 192.168.1.2]

步驟 2: 排序
  sorted = [10.0.0.1, 10.0.0.2, 10.0.0.5, 192.168.1.1, 192.168.1.2]

步驟 3: 選擇中位數
  中位數位置 = 5 / 2 = 2
  分割值 = 10.0.0.5

結果：創建測試節點
  測試: src_ip == 10.0.0.5?
  
  左子樹（不匹配）: [R1, R2]
    → src_ip < 10.0.0.5
  
  右子樹（匹配）: [R3, R4, R5]
    → src_ip >= 10.0.0.5

為什麼選中位數？
  → 試圖平衡左右子樹的大小
  → 避免極度不平衡的樹
```

### 進階策略

#### 策略 1：最佳分割點

```
目標：最小化子樹的不平衡度

for each 唯一值 in 值列表 {
    假設用這個值分割;
    計算左右子樹的大小差異;
    記錄不平衡度;
}

選擇不平衡度最小的分割點;

範例:
  值: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
  
  分割點 = 3:
    左: [1,2]    (2個)
    右: [4,5,6,7,8,9,10]  (7個)
    不平衡度: |2-7| = 5  ❌
  
  分割點 = 5:
    左: [1,2,3,4]  (4個)
    右: [6,7,8,9,10]  (5個)
    不平衡度: |4-5| = 1  ✓ 最佳！
```

#### 策略 2：多維考慮

```
不只看值的分布，還要考慮：
  1. 規則的優先級分布
  2. 查詢的頻率分布
  3. 欄位的區分度

範例：
  如果 80% 的查詢匹配少數規則
  → 應該把這些規則放在淺層
  → 而不是追求完美平衡
```

---

## 🧪 測試類型決定

### 三種測試類型

#### 1. 精確匹配 (EXACT)

```c
// 測試: field == value?
match = (flow_value == test_value);

適用場景:
  - 離散值（如：埠號、協議號）
  - 值分布分散
  
範例:
  測試: dst_port == 80?
  
  匹配: dst_port = 80      → 走右邊
  不匹配: dst_port = 443   → 走左邊
  不匹配: dst_port = 22    → 走左邊
```

#### 2. 前綴匹配 (PREFIX)

```c
// 測試: field in prefix/plen?
uint32_t mask = ~0u << (32 - plen);
match = ((flow_value & mask) == (prefix & mask));

適用場景:
  - IP 地址
  - 值有共同前綴
  
範例:
  測試: src_ip in 10.0.0.0/8?
  
  匹配: 10.0.0.1    → 走右邊
  匹配: 10.255.255.255  → 走右邊
  不匹配: 192.168.1.1  → 走左邊
```

#### 3. 範圍匹配 (RANGE - 未實現)

```c
// 測試: min <= field <= max?
match = (flow_value >= min && flow_value <= max);

適用場景:
  - 連續值範圍
  - 埠號範圍
  
範例:
  測試: 1024 <= src_port <= 65535?
  
  匹配: src_port = 12345  → 走右邊
  不匹配: src_port = 80   → 走左邊
```

### 決定測試類型的邏輯

```c
function 決定測試類型(欄位, 值列表) {
    if (欄位是 IP 地址) {
        if (值有共同前綴) {
            return PREFIX;  // 前綴匹配
        }
    }
    
    if (值分布連續) {
        return RANGE;  // 範圍匹配（未實現）
    }
    
    return EXACT;  // 默認：精確匹配
}
```

### 範例：測試類型選擇

```
場景 1: IP 地址，有共同前綴
  值: [10.0.0.1, 10.0.0.2, 10.0.0.3, 10.0.0.4]
  
  分析: 都在 10.0.0.0/24 範圍內
  選擇: PREFIX 測試
  結果: "src_ip in 10.0.0.0/24?"

場景 2: 端口號，離散分布
  值: [22, 80, 443, 3306, 5432]
  
  分析: 值分散，沒有明顯模式
  選擇: EXACT 測試
  結果: "dst_port == 80?"

場景 3: 端口號，連續範圍
  值: [8000, 8001, 8002, 8003, ..., 8999]
  
  分析: 值連續
  選擇: RANGE 測試（如果實現）
  結果: "8000 <= dst_port <= 8999?"
```

---

## 📝 實際範例

### 完整樹構建範例

```
輸入規則集:
  R1: match src_ip=10.0.0.1, dst_port=80,  priority=100
  R2: match src_ip=10.0.0.2, dst_port=80,  priority=90
  R3: match src_ip=10.0.0.3, dst_port=443, priority=80
  R4: match dst_ip=192.168.1.1,           priority=70
  R5: match (catchall),                   priority=50

步驟 1: 選擇分割欄位
  統計:
    - src_ip: 3 次 (R1, R2, R3)
    - dst_ip: 1 次 (R4)
    - dst_port: 3 次 (R1, R2, R3)
  
  選擇: dst_port (計數=3，且可能分割效果好)

步驟 2: 選擇分割值
  收集值: [80, 80, 443]
  去重排序: [80, 443]
  中位數: 80
  
  測試類型: EXACT（離散值）

步驟 3: 創建根節點
  [Root: dst_port == 80?]

步驟 4: 分割規則集
  左子集（不匹配 80）: [R3, R4, R5]
  右子集（匹配 80）: [R1, R2, R5]
  
  注意: R5 (catchall) 兩邊都有

步驟 5: 遞歸構建左子樹
  輸入: [R3, R4, R5]
  
  選擇欄位: src_ip (R3 使用)
  分割值: 10.0.0.3
  
  創建節點: [src_ip == 10.0.0.3?]
    左: [R4, R5]  → 葉節點
    右: [R3, R5]  → 葉節點

步驟 6: 遞歸構建右子樹
  輸入: [R1, R2, R5]
  
  選擇欄位: src_ip
  分割值: 10.0.0.1
  
  創建節點: [src_ip == 10.0.0.1?]
    左: [R2, R5]  → 葉節點
    右: [R1, R5]  → 葉節點

最終樹結構:
                [Root: dst_port == 80?]
                /                      \
        [dst_port != 80]          [dst_port == 80]
               /                           \
    [src_ip == 10.0.0.3?]         [src_ip == 10.0.0.1?]
       /              \               /              \
   [Leaf: R4,R5]  [Leaf: R3,R5]  [Leaf: R2,R5]  [Leaf: R1,R5]
   p=70,50        p=80,50        p=90,50        p=100,50
```

### 查找流程範例

```
查找: flow(src_ip=10.0.0.1, dst_port=80)

步驟 1: 到達 Root
  測試: dst_port == 80?
  flow.dst_port = 80
  結果: YES → 走右邊

步驟 2: 到達右子節點
  測試: src_ip == 10.0.0.1?
  flow.src_ip = 10.0.0.1
  結果: YES → 走右邊

步驟 3: 到達葉節點 [Leaf: R1,R5]
  規則列表: [R1(p=100), R5(p=50)]
  
  選擇: R1 (優先級最高)

返回: R1 ✅

總測試次數: 2 次
（如果線性搜尋: 最多 5 次）
```

---

## 🚀 優化策略

### 當前可以改進的地方

#### 1. 使用實際值而非 Dummy 值

**當前**:
```c
values[n_values++] = htonl(n_values);  // dummy
```

**改進**:
```c
union mf_value value, mask;
mf_get(&field->id, &rule->match, &value, &mask);
if (!is_all_zeros(&mask, field->n_bytes)) {
    values[n_values++] = value.be32;  // 實際值
}
```

**效果**: 分割更合理，樹更平衡

#### 2. 實現資訊增益算法

**當前**:
```c
// 簡單計數
field_counts[i]++;
```

**改進**:
```c
// 計算資訊增益
double gain = calculate_information_gain(field, rules);
if (gain > best_gain) {
    best_field = field;
    best_gain = gain;
}
```

**效果**: 選擇更優的分割欄位

#### 3. 考慮查詢頻率

**改進**:
```c
// 如果某些規則查詢頻率更高
// 應該把它們放在淺層

for each 規則 {
    權重 = 查詢頻率 * 優先級;
    加權考慮這個規則;
}
```

**效果**: 常用規則查找更快

#### 4. 動態調整策略

**改進**:
```c
// 根據樹的性能統計動態調整

if (平均查找深度 > 閾值) {
    重建樹，使用不同的分割策略;
}
```

**效果**: 適應工作負載變化

---

## 📊 決策質量評估

### 好的決策特徵

```
✅ 1. 平衡性
  左右子樹大小相近
  |左| ≈ |右|

✅ 2. 區分度高
  分割後的子集差異明顯
  重疊少

✅ 3. 深度淺
  樹的深度 ≈ O(log n)
  不是 O(n)

✅ 4. 常用規則在淺層
  高頻查詢快速返回
```

### 評估指標

```
1. 平衡因子
   BF = |左子樹大小 - 右子樹大小|
   BF 越小越好

2. 平均查找深度
   AvgDepth = Σ (查找深度 * 查詢頻率) / 總查詢數
   越小越好

3. 最大深度
   MaxDepth = 從根到最深葉節點的距離
   應該 ≈ log2(n)

4. 葉節點規則數方差
   Var = Σ (葉節點規則數 - 平均值)²
   越小越均勻
```

---

## 🔍 調試和驗證

### 如何檢查決策是否合理？

```bash
# 方法 1: 打印樹結構
./dt-test --print-tree

輸出:
  [Root: dst_port == 80?]
    Left (5 rules):
      [Node: src_ip == 10.0.0.1?]
        Left (2 rules): Leaf
        Right (3 rules): Leaf
    Right (4 rules): Leaf

檢查:
  ✓ 左右是否平衡？
  ✓ 深度是否合理？
  ✓ 分割欄位是否有意義？

# 方法 2: 統計查找路徑
./dt-test --profile-lookups

輸出:
  Average path length: 2.3
  Max path length: 4
  Min path length: 1
  
  Path length distribution:
    1: 20%
    2: 45%
    3: 30%
    4: 5%

檢查:
  ✓ 平均路徑長度 ≈ log2(規則數)?
  ✓ 分布是否集中?

# 方法 3: 可視化樹
./dt-test --visualize > tree.dot
dot -Tpng tree.dot -o tree.png

檢查:
  ✓ 視覺上是否平衡?
  ✓ 有沒有極度不平衡的分支?
```

---

## 💡 關鍵洞察

### 1. 權衡：精確 vs 快速

```
追求最優決策（資訊增益）:
  → 構建時間長
  → 樹可能更優
  
使用簡單啟發式（使用頻率）:
  → 構建快速
  → 樹通常也不錯
  
結論: 對於大多數場景，簡單啟發式已經足夠 ✓
```

### 2. 在線 vs 批量構建

```
在線插入（當前實現）:
  → 逐個插入規則
  → 樹可能不是最優
  → 但增量更新方便
  
批量構建（dt_build_tree）:
  → 一次性構建整棵樹
  → 可以全局優化
  → 但需要重建整棵樹
  
結論: 兩種方法都有用處，看使用場景 ✓
```

### 3. 理論 vs 實踐

```
理論上最優:
  → 資訊增益、基尼係數、完美平衡
  
實踐中夠用:
  → 簡單啟發式、局部優化
  
原因:
  → 真實規則集通常有結構（不是隨機）
  → 過度優化收益遞減
  → 簡單方法更容易維護
  
結論: Keep it simple, but make it work ✓
```

---

## 📚 總結

### 決策規則的核心要素

```
1. 欄位選擇
   ❓ 測試哪個欄位？
   ✅ 使用頻率最高的（當前）
   ✅ 資訊增益最大的（理想）

2. 分割值選擇
   ❓ 測試什麼值？
   ✅ 中位數（當前簡化）
   ✅ 最佳平衡點（理想）

3. 測試類型選擇
   ❓ 如何測試？
   ✅ 精確匹配（離散值）
   ✅ 前綴匹配（IP地址）
   ✅ 範圍匹配（連續值，待實現）

4. 質量評估
   ❓ 決策好不好？
   ✅ 平衡性（左右大小）
   ✅ 深度（查找效率）
   ✅ 實際性能（查詢統計）
```

### 改進優先級

```
P0 (立即):
  ✅ 使用實際值而非 dummy 值
  ✅ 修復插入時的分支選擇

P1 (短期):
  📋 實現資訊增益算法
  📋 完善前綴檢測

P2 (中期):
  📋 實現範圍匹配
  📋 動態樹重建

P3 (長期):
  📋 查詢頻率優化
  📋 多維優化策略
```

---

**文檔位置**: 
- 欄位選擇: `lib/dt-classifier.c:602-658`
- 分割值選擇: `lib/dt-classifier.c:666-691`
- 測試執行: `lib/dt-classifier.c:236-267`
- 樹構建: `lib/dt-classifier.c:693-730`
