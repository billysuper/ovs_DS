# Decision Tree Classifier Implementation

## 概述
本專案實現了 Open vSwitch (OVS) 的決策樹分類器 (Decision Tree Classifier)，作為現有 Tuple Space Search (TSS) 演算法的替代方案。

## 已完成的工作

### 1. 核心數據結構 (dt-classifier.h)

#### dt_node - 統一節點結構
```c
struct dt_node {
    enum dt_node_type type;  // DT_NODE_INTERNAL 或 DT_NODE_LEAF
    union {
        struct dt_internal_node internal;
        struct dt_leaf_node leaf;
    };
};
```

#### dt_internal_node - 內部節點
- 支持多種測試類型：exact, prefix, range
- 使用 RCU 保護的左右子節點指針
- 基於 mf_field 進行條件測試

#### dt_leaf_node - 葉節點
- `rules`: 直接匹配的規則列表
- `inherited_rules`: 從祖先繼承的 wildcard 規則
- `required_mask`: 到達此葉節點所需的最小 mask
- `n_rules`, `n_inherited`: 規則計數器

#### dt_path - 路徑追蹤結構
- 用於 COW 操作的路徑記錄
- 支持最多 64 層深度
- 記錄每個節點和方向（left/right）

### 2. RCU 和 COW 機制

#### dt_node_copy() - 節點複製
- 內部節點：淺拷貝（只複製結構，子節點指針保留）
- 葉節點：深拷貝（完整複製規則列表）
- 確保並發讀者看到一致的樹狀態

#### dt_path_rebuild_cow() - 路徑重建
- 從葉節點向上複製整條路徑
- 每個祖先節點都被複製
- 更新父節點指向新複製的子節點
- 返回新的根節點

#### RCU 延遲釋放
```c
ovsrcu_set(&dt->root, new_root);       // 原子切換根節點
ovsrcu_postpone(dt_node_destroy, old_root);  // 延遲釋放舊樹
```

### 3. 查找功能

#### dt_lookup() - RCU 保護的查找
```c
const struct cls_rule *dt_lookup(
    const struct decision_tree *dt,
    ovs_version_t version,
    const struct flow *flow,
    struct flow_wildcards *wc
);
```

功能特點：
- 從根節點開始遍歷決策樹
- 每個內部節點進行條件測試
- 正確累積 wildcards（un-wildcard 測試的欄位）
- 到達葉節點後檢查直接規則和繼承規則
- 選擇最高優先級且對指定版本可見的規則
- 返回匹配規則的同時更新 flow_wildcards

### 4. 插入功能

#### dt_insert_rule() - 帶 COW 的插入
```c
bool dt_insert_rule(
    struct decision_tree *dt,
    const struct cls_rule *rule,
    ovs_version_t version
);
```

步驟：
1. 記錄從根到目標葉節點的路徑
2. 複製目標葉節點
3. 將新規則按優先級插入規則列表
4. 使用 dt_path_rebuild_cow() 重建路徑
5. 原子切換根節點
6. RCU 延遲釋放舊樹

### 5. 刪除功能

#### dt_remove_rule() - 帶 COW 的刪除
```c
bool dt_remove_rule(
    struct decision_tree *dt,
    const struct cls_rule *rule
);
```

步驟：
1. 定位包含規則的葉節點
2. 複製該葉節點
3. 從規則列表中移除規則
4. 重建路徑
5. 原子切換根節點
6. RCU 延遲釋放

### 6. 樹構建演算法

#### dt_build_tree() - 批量構建
```c
struct dt_node *dt_build_tree(
    struct rculist *rules,
    size_t n_rules,
    size_t max_leaf_size
);
```

策略：
- **欄位選擇**: dt_select_split_field()
  - 基於欄位使用頻率
  - 優先選擇最常被規則匹配的欄位
  - 候選欄位：IN_PORT, ETH_TYPE, IPV4_SRC/DST, IP_PROTO, TCP/UDP_SRC/DST

- **分割值選擇**: dt_find_split_value()
  - 收集該欄位的所有值
  - 使用中值策略
  - 支持 exact 和 prefix 匹配

- **遞歸構建**:
  - 如果規則數 ≤ max_leaf_size：創建葉節點
  - 否則選擇分割欄位和值
  - 將規則分為左右兩組
  - 遞歸構建左右子樹

## 關鍵設計決策

### 1. 為什麼需要 COW 整條路徑？

**問題**: 多層級指針導致的不一致性
```
根節點 -> 內部節點 A -> 內部節點 B -> 葉節點
```

如果只複製葉節點，讀者可能會：
1. 讀取舊的內部節點 B（指向舊葉節點）
2. 寫者更新內部節點 B（指向新葉節點）
3. 讀者繼續使用舊葉節點的數據
4. 舊葉節點被釋放 → 讀者訪問已釋放的內存！

**解決方案**: COW 整條路徑
- 複製從葉節點到根的所有節點
- 一次原子操作切換根節點
- 舊樹保持完整直到所有讀者完成

### 2. TSS vs Decision Tree 比較

| 特性 | TSS | Decision Tree |
|------|-----|---------------|
| 結構 | 分散式（多個 subtables） | 層次式（單一樹） |
| RCU 粒度 | 細粒度（cmap/ccmap 獨立） | 粗粒度（整棵樹 COW） |
| 修改成本 | O(1) | O(tree depth) |
| 適用場景 | 高 wildcard 規則 | 精確匹配為主 |
| 內存使用 | 較少（規則不重複） | 較多（wildcard 繼承） |

### 3. Wildcard 繼承機制

決策樹的葉節點包含兩個規則列表：
- **rules**: 精確匹配的規則
- **inherited_rules**: 從祖先繼承的 wildcard 規則

這避免了將高 wildcard 規則複製到所有葉節點，減少內存開銷。

## 文件清單

### 核心實現
- `lib/dt-classifier.h` - 頭文件，數據結構定義
- `lib/dt-classifier.c` - 實現文件，約 860 行代碼

### 測試
- `lib/dt-classifier-test.c` - 基本功能測試

### 已修改文件
- `lib/automake.mk` - 已包含 dt-classifier.c

## 下一步工作

### 1. 編譯和測試 ✓
需要在 Linux/Unix 環境或 Windows 的 MinGW/Cygwin 環境下編譯：
```bash
cd /path/to/ovs
./configure
make lib/dt-classifier.lo
```

### 2. 運行基本測試
```bash
# 編譯測試程序
gcc -o dt-test lib/dt-classifier-test.c lib/dt-classifier.c \
    -I include -I lib -I . \
    -lssl -lcrypto -lpthread

# 運行測試
./dt-test
```

### 3. 整合到 classifier 介面
需要修改 `lib/classifier.c` 和 `lib/classifier.h`：

選項 A: 編譯時選擇
```c
#ifdef USE_DECISION_TREE
  #define classifier_init dt_classifier_init
  #define classifier_lookup dt_lookup
  // ...
#endif
```

選項 B: 運行時選擇
```c
struct classifier_ops {
    void (*init)(struct classifier *);
    const struct cls_rule *(*lookup)(...);
    bool (*insert)(...);
    bool (*remove)(...);
};

extern struct classifier_ops tss_ops;
extern struct classifier_ops dt_ops;
```

### 4. 性能測試
創建 benchmark 對比：
- 查找性能（不同規則集大小）
- 插入性能（批量 vs 單個）
- 刪除性能
- 內存使用
- 並發性能（多讀者）

### 5. 優化機會
- 更好的欄位選擇算法（信息增益、基尼不純度）
- 樹平衡策略
- Conjunction match 支持（需要相鄰葉節點索引）
- 批量更新優化（一次 COW 應用多個修改）
- 內存池管理（減少分配開銷）

## 編譯問題排查

### 常見錯誤

1. **cannot open source file "linux/netlink.h"**
   - 這是 IDE 配置問題，實際編譯不影響
   - 確保在 Linux 環境或使用正確的交叉編譯工具鏈

2. **undefined reference to `mf_get_value`**
   - 需要鏈接完整的 OVS 庫
   - 確保包含 `lib/meta-flow.c` 等相關文件

3. **undefined reference to `ovsrcu_*`**
   - 需要鏈接 `lib/ovs-rcu.c`

### 完整編譯命令參考
```bash
# 使用 OVS 的構建系統
./boot.sh
./configure
make

# 或單獨編譯測試
gcc -o dt-test \
    lib/dt-classifier-test.c \
    lib/dt-classifier.c \
    lib/classifier.c \
    lib/match.c \
    lib/meta-flow.c \
    lib/flow.c \
    lib/ovs-rcu.c \
    lib/util.c \
    # ... 其他依賴文件 ...
    -I include -I lib -I . \
    -lpthread -lssl -lcrypto
```

## 技術文檔

### COW 路徑複製詳解
```c
// 修改前的樹
Root -> A -> B -> Leaf (要修改)

// 路徑記錄
path = [Root, A, B, Leaf]
directions = [right, left, right]

// COW 過程
1. new_leaf = copy(Leaf) + 修改
2. new_B = copy(B), new_B->right = new_leaf
3. new_A = copy(A), new_A->left = new_B
4. new_Root = copy(Root), new_Root->right = new_A
5. ovsrcu_set(&dt->root, new_Root)
6. ovsrcu_postpone(destroy, old_Root)

// 並發讀者
- 在步驟 5 前：看到完整的舊樹
- 在步驟 5 後：看到完整的新樹
- 永遠不會看到半新半舊的混合狀態
```

### Wildcard 累積機制
```c
// 初始狀態：全部 wildcarded
wc.masks = all_zeros

// 經過內部節點 1（測試 IPV4_SRC）
mf_set(MFF_IPV4_SRC, &all_ones_mask, &wc.masks)
// wc.masks.ipv4_src = 0xffffffff

// 經過內部節點 2（測試 TCP_DST prefix /16）
mask.be32[0] = htonl(0xffff0000)
mf_set(MFF_TCP_DST, &mask, &wc.masks)
// wc.masks.tcp_dst = 0xffff0000

// 到達葉節點，匹配規則的 mask
flow_wildcards_fold_minimatch(&wc, &rule->match)
// 進一步 un-wildcard 規則關心的其他欄位

// 最終 wc.masks 包含所有影響分類決策的欄位
```

## 貢獻者筆記

本實現是 OVS classifier 的實驗性替代方案。主要目標是探索決策樹演算法在 OpenFlow 規則分類中的可行性。

**優點**:
- 對精確匹配規則效率更高
- 樹結構更直觀，易於理解和調試
- 可能在特定場景下提供更好的緩存局部性

**挑戰**:
- COW 路徑複製的內存開銷
- 高 wildcard 規則的處理（繼承機制）
- Conjunction match 的實現複雜性
- 與現有 TSS 的性能對比

**建議**:
- 保留 TSS 作為默認實現
- 通過配置選項或編譯標誌啟用 DT
- 進行充分的性能測試再考慮替換
- 考慮混合架構（高 wildcard 用 TSS，低 wildcard 用 DT）

---
*最後更新: 2025-10-16*
*版本: 1.0*
