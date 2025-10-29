# 決策樹測試結果 - 2025-10-26

## ✅ 編譯成功

所有警告已修正：
- `match_matches_flow` → `minimatch_matches_flow`
- 格式化字符串 `%d` → `%zu`
- 函數簽名改為 `struct ovs_cmdl_context *ctx`
- dt-classifier.c 已加入 lib/automake.mk

## ✅ 通過的測試 (3/6)

1. **test_dt_empty** ✓ - 空樹查找正確返回 NULL
2. **test_dt_single_rule** ✓ - 單一規則匹配和不匹配都正確
3. **test_dt_priority_ordering** ✓ - 優先權排序正確

## ❌ 失敗的測試

### test_dt_dual_classifier
**問題**: 6400 個查找中有大量錯誤

**錯誤模式**:
1. DT 返回 NULL，Simple 找到匹配 (最常見)
2. DT 和 Simple 返回不同優先權的規則

**根本原因分析**:

決策樹構建時使用的欄位選擇策略過於簡單：

```c
static const enum mf_field_id candidate_fields[] = {
    MFF_IPV4_SRC,    // IP 來源
    MFF_IPV4_DST,    // IP 目的
    MFF_TCP_SRC,     // TCP 來源埠 ⚠️
    MFF_TCP_DST,     // TCP 目的埠 ⚠️
    MFF_UDP_SRC,     // UDP 來源埠 ⚠️
    MFF_UDP_DST,     // UDP 目的埠 ⚠️
    MFF_IP_PROTO,    // 協定
    MFF_IN_PORT,     // 輸入埠
    MFF_ETH_TYPE,    // 以太網類型
};
```

**問題**: 當樹使用 `MFF_TCP_DST` 作為分割欄位時，對於非 TCP 流量（UDP/ICMP），該欄位值為 0，導致錯誤分類。

**示例**:
- Flow 2: UDP 流量，決策樹在 `tcp_dst` 節點錯誤分支
- Flow 4: 規則優先權 48 vs 1004 - 樹選錯了葉節點中的規則

## 📊 測試統計

```
樹結構 (50 規則測試):
- Total rules: 50
- Internal nodes: 4
- Leaf nodes: 5
- Max depth: 3
- Average rules per leaf: 10.00

樹拓撲:
ROOT: tcp_dst 分割
  ├─ L: nw_proto 分割
  │   ├─ L: in_port 分割 (7 + 19 規則)
  │   └─ R: 16 規則
  └─ R: tcp_dst 分割 (4 + 4 規則)
```

## 🔧 需要修復的問題

### 1. 欄位選擇策略（高優先權）

**當前**: 選擇「最多規則關心的欄位」
```c
if (!is_all_zeros(&mask, field->n_bytes)) {
    field_counts[i]++;  // 簡單計數
}
```

**需要**: 考慮欄位的有效性
- TCP/UDP 埠欄位僅對相應協定有效
- 應該檢查 `dl_type` 和 `nw_proto` 前置條件

**建議修正**:
```c
// 檢查欄位是否對此規則有效
if (field_is_valid_for_rule(field, rule)) {
    if (!is_all_zeros(&mask, field->n_bytes)) {
        field_counts[i]++;
    }
}
```

### 2. 樹遍歷邏輯（中優先權）

**當前**: 簡單檢查欄位值
```c
mf_get_value(field, flow, &value);
match = (ntohl(value.be32) == ntohl(split_value));
```

**需要**: 處理欄位無效的情況
- 對於非 TCP 流量訪問 `tcp_dst` 應該有預設行為
- 可能需要「不適用」分支

### 3. 葉節點匹配（已部分修正）

**已修正**: 現在使用 `minimatch_matches_flow()` 檢查每個規則
```c
if (minimatch_matches_flow(&rule->match, flow)) {
    // 選擇最高優先權
}
```

**但**: 這導致效能下降（O(rules_in_leaf)），本應該是決策樹的優勢

## 📝 建議的修復順序

### 階段 1: 快速修復（讓測試通過）
1. 限制候選欄位為「通用欄位」：
   - `MFF_IN_PORT`, `MFF_ETH_TYPE`, `MFF_IP_PROTO`
   - `MFF_IPV4_SRC`, `MFF_IPV4_DST`
   - 移除協定特定欄位（TCP/UDP 埠）

2. 改進分割值選擇：
   - 排序值並選擇真正的中位數
   - 確保分割能實際劃分規則

### 階段 2: 正確實現（中期）
1. 實現欄位有效性檢查
2. 實現資訊增益計算（Information Gain）
3. 支援多層決策（協定 → 協定特定欄位）

### 階段 3: 優化（長期）
1. 優化葉節點（預排序規則）
2. 實現規則子集索引
3. 支援前綴匹配和範圍匹配

## 🎯 當前成就

### ✅ 已完成
- 完整的測試框架（650 行）
- 雙分類器驗證機制
- 6 個測試案例
- 成功編譯並執行
- 基本功能正確（3/6 測試通過）

### ⏳ 進行中
- 欄位選擇策略改進
- 匹配邏輯優化

### 📋 待辦
- 完整通過所有測試
- 效能優化
- 更多測試案例（刪除、並發等）

## 💡 關鍵發現

1. **測試成功發現了真實問題** - 雙分類器驗證非常有效
2. **問題可修復** - 不是根本設計缺陷，是實現細節問題
3. **測試驅動開發** - 測試幫助我們快速定位問題

## 下一步行動

推薦：
```bash
# 修改 dt-classifier.c 中的 candidate_fields
# 移除 TCP/UDP 埠，只保留通用欄位

# 重新編譯
cd /mnt/d/ovs_DS
make tests/ovstest

# 運行測試
./tests/ovstest test-dt-classifier
```

預期：前 4 個測試應該能通過。
