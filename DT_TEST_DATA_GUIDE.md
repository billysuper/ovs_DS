# DT 分類器測試數據指南

**創建日期**: 2025-10-17  
**目的**: 分析當前 DT 分類器支持的測試數據類型和推薦測試場景

---

## 📊 當前測試覆蓋情況

### ✅ 已測試的數據類型

#### 1. **基礎結構測試** (dt-test-minimal.c)
```c
測試內容: 空樹初始化和銷毀
數據特點:
  - 無規則插入
  - 僅測試結構完整性
  - 驗證內存分配/釋放

適用場景: 驗證基礎框架
狀態: ✅ 100% PASS
```

#### 2. **單一規則測試** (dt-test-ultra-simple.c)
```c
測試數據:
  - 規則類型: Catchall（匹配所有流量）
  - 優先級: 100
  - Match 條件: 無特定條件（match_init_catchall）

實際測試:
  struct match match;
  match_init_catchall(&match);  // 匹配所有
  cls_rule_init(rule, &match, 100);

適用場景: 最簡單的插入/查找
狀態: ✅ 100% PASS
```

#### 3. **簡單欄位匹配** (dt-classifier-test.c)
```c
測試數據類型:
  a) 入口埠號 (in_port)
     - match_set_in_port(&match, 1)
     - 測試: 單一埠匹配
  
  b) 以太網類型 (dl_type)
     - match_set_dl_type(&match, htons(ETH_TYPE_IP))
     - 測試: IPv4 封包識別
  
  c) 組合條件
     - in_port=1 AND dl_type=IPv4
     - 測試: 多欄位匹配

適用場景: L2/L3 基礎分類
狀態: ✅ 100% PASS
```

#### 4. **多規則場景** (dt-classifier-test.c)
```c
測試數據:
  - 規則數量: 5個
  - 優先級範圍: 10, 20, 30, 40, 50
  - Match 條件: 不同的 in_port (1-5)

實際結構:
  Rule 1: in_port=1, priority=10
  Rule 2: in_port=2, priority=20
  Rule 3: in_port=3, priority=30
  Rule 4: in_port=4, priority=40
  Rule 5: in_port=5, priority=50

適用場景: 基礎優先級處理
狀態: ✅ 插入通過，樹結構正確
```

---

## 🎯 推薦的測試數據類型

### 優先級 1 (立即可測) - 簡單欄位

#### A. L2 欄位測試
```c
適合測試的欄位:
  ✅ in_port      - 入口埠號 (已測試)
  ✅ dl_type      - 以太網類型 (已測試)
  ⭐ dl_src       - 源 MAC 地址
  ⭐ dl_dst       - 目標 MAC 地址
  ⭐ dl_vlan      - VLAN ID
  ⭐ dl_vlan_pcp  - VLAN 優先級

示例測試數據:
  // MAC 地址過濾
  struct eth_addr mac = ETH_ADDR_C(00,11,22,33,44,55);
  match_set_dl_src(&match, mac);
  
  // VLAN 過濾
  match_set_dl_vlan(&match, htons(100));  // VLAN ID 100
  
  // 乙太網類型
  match_set_dl_type(&match, htons(ETH_TYPE_ARP));  // ARP 封包
```

#### B. L3 欄位測試 (IPv4)
```c
適合測試的欄位:
  ⭐ nw_src       - 源 IP 地址
  ⭐ nw_dst       - 目標 IP 地址
  ⭐ nw_proto     - IP 協議 (TCP/UDP/ICMP)
  ⭐ nw_tos       - ToS/DSCP

示例測試數據:
  // IP 地址過濾
  match_set_nw_src(&match, htonl(0x0a000001));  // 10.0.0.1
  match_set_nw_dst(&match, htonl(0xc0a80101));  // 192.168.1.1
  
  // 協議過濾
  match_set_nw_proto(&match, IPPROTO_TCP);  // TCP 封包
```

#### C. L4 欄位測試 (TCP/UDP)
```c
適合測試的欄位:
  ⭐ tp_src       - 源埠號
  ⭐ tp_dst       - 目標埠號

示例測試數據:
  // 埠號過濾
  match_set_tp_src(&match, htons(80));   // HTTP 源埠
  match_set_tp_dst(&match, htons(443));  // HTTPS 目標埠
```

### 優先級 2 (基礎功能完善後) - 複雜場景

#### D. 精確匹配測試
```c
測試場景: 5-tuple 匹配（典型防火牆規則）
  - 源 IP
  - 目標 IP
  - 協議
  - 源埠
  - 目標埠

示例數據:
  match_set_dl_type(&match, htons(ETH_TYPE_IP));
  match_set_nw_src(&match, htonl(0x0a000001));
  match_set_nw_dst(&match, htonl(0xc0a80101));
  match_set_nw_proto(&match, IPPROTO_TCP);
  match_set_tp_src(&match, htons(12345));
  match_set_tp_dst(&match, htons(80));

預期行為: 僅匹配完全符合的流量
```

#### E. 前綴匹配測試
```c
測試場景: 子網匹配
  - 10.0.0.0/8
  - 192.168.1.0/24
  - 172.16.0.0/16

示例數據:
  match_set_nw_src_masked(&match, 
                          htonl(0x0a000000),    // 10.0.0.0
                          htonl(0xff000000));   // /8 mask

預期行為: 匹配整個子網範圍
注意: 目前 DT 實現可能需要完善 wildcard 追蹤
```

#### F. 優先級衝突測試
```c
測試場景: 多規則重疊匹配
  Rule 1: in_port=1, priority=100
  Rule 2: in_port=*, priority=50    (catchall)
  Flow:   in_port=1

預期結果: 應該匹配 Rule 1（高優先級）

測試數據:
  // 高優先級特定規則
  struct match match1;
  match_init_catchall(&match1);
  match_set_in_port(&match1, 1);
  cls_rule_init(rule1, &match1, 100);
  
  // 低優先級通用規則
  struct match match2;
  match_init_catchall(&match2);
  cls_rule_init(rule2, &match2, 50);
```

### 優先級 3 (進階功能) - 壓力測試

#### G. 大量規則測試
```c
測試場景: 不同規模的規則集
  - Small: 10-100 規則
  - Medium: 100-1000 規則
  - Large: 1000-10000 規則

示例數據:
  for (int i = 0; i < N_RULES; i++) {
      struct match match;
      match_init_catchall(&match);
      
      // 使用不同欄位組合避免完全重複
      match_set_nw_src(&match, htonl(0x0a000000 + i));
      match_set_tp_dst(&match, htons(1000 + i));
      
      cls_rule_init(rule, &match, i);
      dt_insert_rule(&dt, rule, version);
  }

測量指標:
  - 插入時間
  - 查找時間
  - 內存使用
  - 樹深度/平衡性
```

#### H. 真實流量模擬
```c
測試場景: 模擬真實網路流量
  - Web 流量 (HTTP/HTTPS)
  - DNS 查詢
  - SSH 連線
  - P2P 流量

示例數據集:
  // HTTP GET 請求
  Flow 1: src=10.0.0.2, dst=93.184.216.34, proto=TCP, sport=54321, dport=80
  
  // DNS 查詢
  Flow 2: src=10.0.0.2, dst=8.8.8.8, proto=UDP, sport=43210, dport=53
  
  // SSH 連線
  Flow 3: src=10.0.0.2, dst=203.0.113.5, proto=TCP, sport=49152, dport=22

預期: DT 能正確分類各種流量類型
```

---

## 🚫 目前不適用的測試數據

### ❌ 不支援或需要完善的功能

#### 1. Conjunction Matches
```c
狀態: ❌ 未實現
原因: lib/dt-classifier.c:240 返回 false

不適用數據:
  // 複雜的 AND/OR/NOT 組合
  match_set_conj_id(&match, conj_id);
  
需要等待: Conjunction 支持實現
```

#### 2. 複雜 Wildcard 追蹤
```c
狀態: ⚠️ 簡化實現
原因: dt_lookup() 使用簡化的 wildcard 處理

受限數據:
  // 需要精確 wildcard 追蹤的場景
  - 部分欄位通配
  - 範圍匹配
  - 複雜子網掩碼

當前行為: 可能返回不完整的 wildcards
影響: 對查找結果正確性影響較小，對 megaflow 優化有影響
```

#### 3. Metadata 欄位
```c
狀態: ⚠️ 未測試
原因: 測試中未涉及 metadata 欄位

未測試數據:
  - metadata (通用元數據欄位)
  - tunnel 相關欄位
  - register 欄位

建議: 基礎功能穩定後再測試
```

#### 4. IPv6 欄位
```c
狀態: ⚠️ 未測試
原因: 當前測試聚焦於 IPv4

未測試數據:
  - ipv6_src
  - ipv6_dst
  - ipv6_label
  - icmpv6_type/code

建議: IPv4 測試完整後再擴展
```

---

## 📝 推薦的測試策略

### 階段 1: 基礎驗證（當前階段）
**時間**: 已完成  
**數據類型**:
- ✅ Catchall 規則
- ✅ 單一欄位 (in_port, dl_type)
- ✅ 基礎多規則 (5個)

**下一步**: 擴展到更多 L2/L3 欄位

### 階段 2: 功能擴展（1-2天）
**數據類型**:
- ⭐ MAC 地址過濾
- ⭐ IP 地址過濾
- ⭐ L4 埠號過濾
- ⭐ 優先級衝突處理
- ⭐ 10-50 規則測試

**目標**: 覆蓋常用 OpenFlow 匹配欄位

### 階段 3: 壓力測試（3-5天）
**數據類型**:
- 100-1000 規則
- 複雜 5-tuple 匹配
- 前綴/子網匹配
- 真實流量模擬

**目標**: 驗證性能和穩定性

### 階段 4: 進階功能（1-2週）
**數據類型**:
- Conjunction matches
- IPv6 支持
- Tunnel 欄位
- Metadata 欄位

**目標**: 功能完整性

---

## 🛠️ 實用測試模板

### 模板 1: 單一欄位測試
```c
void test_single_field(void) {
    struct decision_tree dt;
    dt_init(&dt);
    
    // 創建規則
    struct cls_rule *rule = xmalloc(sizeof *rule);
    struct match match;
    match_init_catchall(&match);
    
    // 設置要測試的欄位（替換這裡）
    match_set_in_port(&match, 1);  // ← 修改此行測試其他欄位
    
    cls_rule_init(rule, &match, 100);
    dt_insert_rule(&dt, rule, 1);
    
    // 創建匹配的流
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.in_port.ofp_port = 1;  // ← 對應修改
    
    // 查找
    const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
    printf("Test result: %s\n", found ? "PASS" : "FAIL");
    
    // 清理
    dt_destroy(&dt);
    cls_rule_destroy(rule);
    free(rule);
}
```

### 模板 2: 多欄位組合測試
```c
void test_multi_field(void) {
    struct decision_tree dt;
    dt_init(&dt);
    
    struct cls_rule *rule = xmalloc(sizeof *rule);
    struct match match;
    match_init_catchall(&match);
    
    // 組合多個欄位
    match_set_dl_type(&match, htons(ETH_TYPE_IP));
    match_set_nw_proto(&match, IPPROTO_TCP);
    match_set_tp_dst(&match, htons(80));
    
    cls_rule_init(rule, &match, 100);
    dt_insert_rule(&dt, rule, 1);
    
    // 創建匹配的流（HTTP 流量）
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.dl_type = htons(ETH_TYPE_IP);
    flow.nw_proto = IPPROTO_TCP;
    flow.tp_dst = htons(80);
    
    const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
    printf("HTTP traffic match: %s\n", found ? "PASS" : "FAIL");
    
    dt_destroy(&dt);
    cls_rule_destroy(rule);
    free(rule);
}
```

### 模板 3: 優先級測試
```c
void test_priority(void) {
    struct decision_tree dt;
    dt_init(&dt);
    
    // 低優先級通用規則
    struct cls_rule *rule_low = xmalloc(sizeof *rule_low);
    struct match match_low;
    match_init_catchall(&match_low);
    cls_rule_init(rule_low, &match_low, 50);
    dt_insert_rule(&dt, rule_low, 1);
    
    // 高優先級特定規則
    struct cls_rule *rule_high = xmalloc(sizeof *rule_high);
    struct match match_high;
    match_init_catchall(&match_high);
    match_set_in_port(&match_high, 1);
    cls_rule_init(rule_high, &match_high, 100);
    dt_insert_rule(&dt, rule_high, 1);
    
    // 查找應該匹配高優先級規則
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.in_port.ofp_port = 1;
    
    const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
    printf("Priority test: %s (found priority=%d, expected=100)\n",
           (found && found->priority == 100) ? "PASS" : "FAIL",
           found ? found->priority : 0);
    
    dt_destroy(&dt);
    cls_rule_destroy(rule_low);
    cls_rule_destroy(rule_high);
    free(rule_low);
    free(rule_high);
}
```

### 模板 4: 批量規則測試
```c
void test_many_rules(int n_rules) {
    struct decision_tree dt;
    dt_init(&dt);
    
    printf("Inserting %d rules...\n", n_rules);
    
    for (int i = 0; i < n_rules; i++) {
        struct cls_rule *rule = xmalloc(sizeof *rule);
        struct match match;
        match_init_catchall(&match);
        
        // 使用不同的 IP 地址確保規則不同
        match_set_nw_src(&match, htonl(0x0a000000 + i));  // 10.0.0.0 + i
        
        cls_rule_init(rule, &match, i);
        bool ok = dt_insert_rule(&dt, rule, 1);
        
        if (!ok) {
            printf("Insert failed at rule %d\n", i);
            break;
        }
    }
    
    // 獲取統計信息
    size_t n_rules_actual, n_internal, n_leaf, max_depth;
    dt_get_stats(&dt, &n_rules_actual, &n_internal, &n_leaf, &max_depth);
    
    printf("Results:\n");
    printf("  Rules: %zu / %d\n", n_rules_actual, n_rules);
    printf("  Internal nodes: %zu\n", n_internal);
    printf("  Leaf nodes: %zu\n", n_leaf);
    printf("  Max depth: %zu\n", max_depth);
    printf("  Test: %s\n", n_rules_actual == n_rules ? "PASS" : "FAIL");
    
    dt_destroy(&dt);
}
```

---

## 📊 測試數據總結表

| 數據類型 | 複雜度 | 當前狀態 | 推薦階段 | 預期結果 |
|---------|--------|---------|---------|---------|
| 空樹 | ⭐ | ✅ 已測試 | 階段1 | PASS |
| Catchall 規則 | ⭐ | ✅ 已測試 | 階段1 | PASS |
| 單一 L2 欄位 | ⭐ | ✅ 已測試 | 階段1 | PASS |
| 單一 L3 欄位 | ⭐⭐ | ⭐ 建議測試 | 階段2 | 應 PASS |
| 單一 L4 欄位 | ⭐⭐ | ⭐ 建議測試 | 階段2 | 應 PASS |
| MAC 地址 | ⭐⭐ | ⭐ 建議測試 | 階段2 | 應 PASS |
| IP 地址 | ⭐⭐ | ⭐ 建議測試 | 階段2 | 應 PASS |
| 5-10 規則 | ⭐⭐ | ✅ 已測試 | 階段1 | PASS |
| 多欄位組合 | ⭐⭐⭐ | ⭐ 建議測試 | 階段2 | 應 PASS |
| 優先級衝突 | ⭐⭐⭐ | ⭐ 建議測試 | 階段2 | 應 PASS |
| 50-100 規則 | ⭐⭐⭐ | 📋 待測試 | 階段3 | 未知 |
| 子網匹配 | ⭐⭐⭐ | 📋 待測試 | 階段3 | 需完善 |
| 1000+ 規則 | ⭐⭐⭐⭐ | 📋 待測試 | 階段3 | 未知 |
| 真實流量 | ⭐⭐⭐⭐ | 📋 待測試 | 階段3 | 未知 |
| Conjunction | ⭐⭐⭐⭐⭐ | ❌ 不支持 | 階段4 | 需實現 |
| IPv6 | ⭐⭐⭐⭐ | 📋 待測試 | 階段4 | 未知 |
| Tunnel 欄位 | ⭐⭐⭐⭐ | 📋 待測試 | 階段4 | 未知 |

**圖例**:
- ✅ 已測試並通過
- ⭐ 推薦立即測試
- 📋 計劃中
- ❌ 不支持/需要實現
- ⚠️ 部分支持

---

## 🎯 建議的下一步測試

### 立即可執行（今天）
1. **MAC 地址測試** - 添加 `dl_src/dl_dst` 測試
2. **IP 地址測試** - 添加 `nw_src/nw_dst` 測試  
3. **埠號測試** - 添加 `tp_src/tp_dst` 測試

**預估時間**: 2-3 小時  
**風險**: 低  
**價值**: 高（覆蓋最常用欄位）

### 短期目標（本週）
4. **優先級衝突測試** - 驗證高優先級優先匹配
5. **50 規則測試** - 擴大規則規模
6. **多欄位組合** - 測試 5-tuple 匹配

**預估時間**: 1 天  
**風險**: 中  
**價值**: 高（驗證核心邏輯）

### 中期目標（下週）
7. **壓力測試** - 100-1000 規則
8. **性能對比** - 與 TSS 對比
9. **真實場景** - HTTP/DNS/SSH 分類

**預估時間**: 3-5 天  
**風險**: 中  
**價值**: 極高（性能數據）

---

## 📖 參考資源

### OVS Match 欄位文檔
```
include/openvswitch/match.h - match_set_* 函數定義
include/flow.h               - struct flow 定義
lib/classifier.c             - TSS 實現參考
lib/match.c                  - Match 輔助函數
```

### 測試示例
```
tests/test-classifier.c      - TSS 的完整測試套件
lib/dt-classifier-test.c     - 當前 DT 測試
```

### 相關文檔
```
DT_PROJECT_SUMMARY.md        - 項目總結
DT_CLASSIFIER_README.md      - 設計文檔
DT_QUICK_REFERENCE.md        - 命令參考
```

---

**建議**: 從「立即可執行」的測試開始，逐步擴展到更複雜的場景。每個階段完成後，記錄測試結果並更新此文檔。
