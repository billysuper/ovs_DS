# oftable 與字段使用的關係

## 關鍵概念澄清 ⚠️

**錯誤理解**: 不同字段拆分到不同 oftable
**正確理解**: **每張表都可以匹配所有字段**，只是 **根據功能階段選擇性匹配不同字段**

---

## 核心原則

### ✅ 每張 oftable 都包含完整的 classifier

```c
struct oftable {
    struct classifier cls;  // 完整的分類器,可以匹配所有字段
};

// classifier 可以匹配的字段 (128+ 個字段):
- in_port, dl_src, dl_dst, dl_vlan, dl_type
- nw_src, nw_dst, nw_proto, tp_src, tp_dst
- tun_id, metadata, reg0-reg15
- ... (所有 OpenFlow 支持的字段)
```

### ❌ 不是字段劃分

```
錯誤理解:
Table 0: 只能匹配 dl_src, dl_dst     ← 錯誤!
Table 1: 只能匹配 in_port, dl_vlan   ← 錯誤!
Table 2: 只能匹配 dl_dst             ← 錯誤!

正確理解:
Table 0: 可以匹配任何字段,但選擇匹配 dl_src, dl_dst (準入控制邏輯需要)
Table 1: 可以匹配任何字段,但選擇匹配 in_port, dl_vlan (VLAN 處理邏輯需要)
Table 2: 可以匹配任何字段,但選擇匹配 dl_src, dl_vlan (MAC 學習邏輯需要)
```

---

## 實際例子：VLAN 交換機

### Table 0: 準入控制 (可以匹配所有字段,選擇匹配 dl_src, dl_dst)

```bash
# 規則 1: 丟棄多播源地址 (匹配 dl_src 字段)
ovs-ofctl add-flow br0 \
  "table=0, priority=100, dl_src=01:00:00:00:00:00/01:00:00:00:00:00, actions=drop"

# 規則 2: 丟棄 STP 包 (匹配 dl_dst 字段)
ovs-ofctl add-flow br0 \
  "table=0, priority=100, dl_dst=01:80:c2:00:00:00/ff:ff:ff:ff:ff:f0, actions=drop"

# 規則 3: 其他包進入 Table 1 (默認規則)
ovs-ofctl add-flow br0 \
  "table=0, priority=0, actions=resubmit(,1)"
```

**為什麼這樣設計？**
- Table 0 的功能是準入控制
- 只需要檢查源/目標 MAC 就能判斷是否丟棄
- 不需要檢查 IP、端口等字段 (與準入控制無關)

### Table 1: VLAN 處理 (可以匹配所有字段,選擇匹配 in_port, vlan_tci)

```bash
# 規則 1: trunk 端口 p1,保持 VLAN (匹配 in_port 字段)
ovs-ofctl add-flow br0 \
  "table=1, priority=99, in_port=1, actions=resubmit(,2)"

# 規則 2: access 端口 p2,打上 VLAN 20 (匹配 in_port + vlan_tci)
ovs-ofctl add-flow br0 \
  "table=1, priority=99, in_port=2, vlan_tci=0, actions=mod_vlan_vid:20,resubmit(,2)"

# 規則 3: access 端口 p3,打上 VLAN 30
ovs-ofctl add-flow br0 \
  "table=1, priority=99, in_port=3, vlan_tci=0, actions=mod_vlan_vid:30,resubmit(,2)"

# 規則 4: 默認丟棄 (帶 VLAN 標籤的包進入 access 端口)
ovs-ofctl add-flow br0 \
  "table=1, priority=0, actions=drop"
```

**為什麼這樣設計？**
- Table 1 的功能是 VLAN 處理
- 需要知道從哪個端口進來 (in_port) 和是否有 VLAN 標籤 (vlan_tci)
- 不需要檢查 MAC 地址、IP 地址等 (與 VLAN 處理無關)

### Table 2: MAC 學習 (可以匹配所有字段,選擇匹配 dl_src, dl_vlan)

```bash
# 單條規則: 學習源 MAC → 入口端口的映射
ovs-ofctl add-flow br0 \
  "table=2, priority=0, actions=learn(
    table=10,                              # 學習到 Table 10
    NXM_OF_VLAN_TCI[0..11],               # 匹配相同 VLAN
    NXM_OF_ETH_DST[]=NXM_OF_ETH_SRC[],    # 目標 MAC = 當前源 MAC
    load:NXM_OF_IN_PORT[]->NXM_NX_REG0[]  # 保存入口端口到寄存器
  ),resubmit(,3)"
```

**為什麼這樣設計？**
- Table 2 的功能是 MAC 學習
- 需要記錄 "源 MAC + VLAN → 入口端口" 的映射
- 使用 learn() 動作動態添加規則到 Table 10

### Table 10: MAC 查找 (動態生成,可以匹配所有字段,選擇匹配 dl_dst, dl_vlan)

```bash
# 這些規則由 Table 2 的 learn() 動作自動生成:

# 學到: MAC 00:11:22:33:44:55 在 VLAN 20 的端口 2
table=10, priority=0, dl_vlan=20, dl_dst=00:11:22:33:44:55, \
  actions=load:2->NXM_NX_REG0[0..15],resubmit(,4)

# 學到: MAC aa:bb:cc:dd:ee:ff 在 VLAN 30 的端口 3
table=10, priority=0, dl_vlan=30, dl_dst=aa:bb:cc:dd:ee:ff, \
  actions=load:3->NXM_NX_REG0[0..15],resubmit(,4)
```

**為什麼這樣設計？**
- Table 10 的功能是 MAC 轉發
- 需要根據 "目標 MAC + VLAN" 查找出口端口
- 如果找到就單播,找不到就在 Table 3 泛洪

### Table 3: 未知目標處理 (可以匹配所有字段,選擇匹配 dl_vlan)

```bash
# 泛洪到同 VLAN 的所有端口
ovs-ofctl add-flow br0 \
  "table=3, priority=99, dl_vlan=20, actions=load:0x1b->NXM_NX_REG0[0..15],resubmit(,4)"

ovs-ofctl add-flow br0 \
  "table=3, priority=99, dl_vlan=30, actions=load:0x1d->NXM_NX_REG0[0..15],resubmit(,4)"
```

### Table 4: 輸出處理 (可以匹配所有字段,選擇匹配 dl_vlan, reg0)

```bash
# trunk 端口 p1,保持 VLAN 標籤
ovs-ofctl add-flow br0 \
  "table=4, priority=99, reg0=0x1, actions=output:1"

# access 端口 p2 (VLAN 20),移除 VLAN 標籤
ovs-ofctl add-flow br0 \
  "table=4, priority=99, dl_vlan=20, reg0=0x2, actions=strip_vlan,output:2"

# access 端口 p3 (VLAN 30),移除 VLAN 標籤
ovs-ofctl add-flow br0 \
  "table=4, priority=99, dl_vlan=30, reg0=0x4, actions=strip_vlan,output:3"

# access 端口 p4 (VLAN 30),移除 VLAN 標籤
ovs-ofctl add-flow br0 \
  "table=4, priority=99, dl_vlan=30, reg0=0x8, actions=strip_vlan,output:4"
```

---

## 字段使用模式總結

| Table | 主要功能 | 匹配的字段 | 不匹配的字段 | 原因 |
|-------|---------|-----------|-------------|------|
| 0 | 準入控制 | dl_src, dl_dst | in_port, VLAN, IP | 只需檢查 MAC 合法性 |
| 1 | VLAN 處理 | in_port, vlan_tci | dl_src, dl_dst, IP | 只需知道端口和 VLAN |
| 2 | MAC 學習 | dl_src, dl_vlan, in_port | dl_dst, IP | 學習源 MAC → 端口 |
| 3 | 未知轉發 | dl_vlan | dl_src, dl_dst, IP | 只需知道 VLAN 泛洪 |
| 4 | 輸出處理 | dl_vlan, reg0 (出口端口) | dl_src, dl_dst, IP | 只需知道 VLAN 和端口 |
| 10 | MAC 查找 | dl_dst, dl_vlan | dl_src, in_port, IP | 查找目標 MAC → 端口 |

**關鍵理解**:
- ✅ 每張表 **能夠** 匹配所有字段 (技術上可行)
- ✅ 每張表 **選擇** 只匹配需要的字段 (設計上合理)
- ✅ 這是 **邏輯設計** 而非技術限制

---

## 為什麼不是按字段劃分？

### 反例 1: Table 2 需要匹配多個字段

```bash
# Table 2 同時需要:
- dl_src (源 MAC)
- dl_vlan (VLAN ID)
- in_port (入口端口)

# 如果按字段劃分,這三個字段應該在不同的表 → 不可行!
```

### 反例 2: 多張表可能匹配相同字段

```bash
# Table 1 匹配 dl_vlan (VLAN 處理)
table=1, in_port=2, vlan_tci=0, actions=mod_vlan_vid:20

# Table 3 也匹配 dl_vlan (未知轉發)
table=3, dl_vlan=20, actions=flood

# Table 4 也匹配 dl_vlan (輸出處理)
table=4, dl_vlan=20, reg0=0x2, actions=strip_vlan,output:2

# 同一個字段在多張表中使用 → 不是字段劃分!
```

### 反例 3: 單張表可以匹配任意字段組合

```bash
# 實際案例: 企業 ACL 表 (Table 0)
table=0, priority=100, in_port=1, dl_src=00:11:22:33:44:55, \
         nw_src=192.168.1.10, nw_dst=10.0.0.0/8, \
         tp_dst=80, actions=drop

# 這條規則同時匹配:
- in_port (端口)
- dl_src (MAC 源地址)
- nw_src (IP 源地址)
- nw_dst (IP 目標地址)
- tp_dst (TCP 目標端口)

# 如果是按字段劃分,這 5 個字段應該在 5 張不同的表 → 不合理!
```

---

## 實際的字段使用策略

### 策略 1: 最小必要字段 (Principle of Least Privilege)

```
每張表只匹配完成其功能所需的最少字段:
  → 減少規則複雜度
  → 提升查找性能
  → 降低誤匹配風險
```

**示例**:
```bash
# Table 0 準入控制: 只檢查 MAC
table=0, dl_src=01:00:00:00:00:00/01:00:00:00:00:00, actions=drop
# 不需要檢查: in_port, VLAN, IP, 端口 (與準入控制無關)

# Table 1 VLAN 處理: 只檢查端口和 VLAN
table=1, in_port=2, vlan_tci=0, actions=mod_vlan_vid:20
# 不需要檢查: MAC, IP (與 VLAN 處理無關)
```

### 策略 2: 漸進式字段累積 (Progressive Field Accumulation)

```
數據包在流水線中前進,不同階段檢查不同字段:

Packet → Table 0 (檢查 MAC) 
       → Table 1 (檢查 VLAN) 
       → Table 2 (檢查 MAC+VLAN) 
       → Table 3 (檢查目標 MAC)

而不是在一張表中檢查所有字段
```

### 策略 3: 功能導向字段選擇 (Functionality-Driven Field Selection)

```
根據表的功能決定匹配哪些字段:

功能: ACL (訪問控制列表)
  → 需要匹配: 源 IP, 目標 IP, 協議, 端口
  → 不需要: VLAN (與訪問控制無關)

功能: 負載均衡
  → 需要匹配: 源/目標 IP+端口 (計算 hash)
  → 不需要: MAC 地址 (與負載均衡無關)

功能: VLAN 隔離
  → 需要匹配: VLAN ID
  → 不需要: IP 地址 (L2 隔離不管 L3)
```

---

## Classifier 的實際行為

### Classifier 內部的字段索引

```c
// lib/classifier.c 中的實現
struct classifier {
    // TSS (Tuple Space Search) 使用多個子表
    // 每個子表基於不同的字段組合
    struct pvector subtables;  // 按匹配字段分組的子表
};

struct cls_subtable {
    // 子表根據規則的匹配字段組合創建
    // 例如: (in_port, dl_vlan) 是一個子表
    //      (dl_src, dl_dst) 是另一個子表
    struct cmap rules;  // 該字段組合的所有規則
    struct minimask mask;  // 該子表匹配的字段掩碼
};
```

**重要**: 
- Classifier 內部 **確實** 會根據字段創建不同的子表 (TSS 優化)
- 但這是 **分類器內部** 的優化,不是 **oftable 之間** 的劃分
- 每個 oftable 都有自己完整的 classifier,可以匹配任何字段組合

### 示例: Table 1 的 Classifier 內部

```c
// Table 1 的 classifier 可能包含多個 subtable:
oftable[1].cls {
    subtable[0]: mask=(in_port, vlan_tci)     // 規則 1-3
    subtable[1]: mask=(in_port)               // 可能的其他規則
    subtable[2]: mask=(dl_src, dl_dst, ...)   // 如果添加了其他規則
}
```

---

## DT 集成的啟示

### DT 需要支持所有字段組合

```c
// DT 必須能夠處理任意字段組合
struct dt_node *dt_build_tree(struct cls_rule **rules, size_t n_rules) {
    // 規則可能匹配:
    // - 只有 dl_src
    // - 只有 in_port + vlan_tci
    // - in_port + dl_src + nw_src + tp_dst (任意組合!)
    
    // DT 需要:
    // 1. 分析所有規則實際使用的字段
    // 2. 選擇最佳分裂字段 (可能是任意字段)
    // 3. 構建能處理所有組合的決策樹
}
```

### 不要假設字段是預先劃分的

```c
// ❌ 錯誤假設
if (table_id == 0) {
    dt_build_tree_for_mac_fields();  // 只考慮 MAC 字段
} else if (table_id == 1) {
    dt_build_tree_for_vlan_fields(); // 只考慮 VLAN 字段
}

// ✅ 正確做法
dt_build_tree(rules, n_rules) {
    // 自動分析規則中實際使用的字段
    used_fields = analyze_rules(rules);
    // 選擇最佳分裂字段 (可能是任意字段)
    best_field = select_split_field(rules, used_fields);
}
```

---

## 總結

| 概念 | 說明 | 正確性 |
|------|------|-------|
| **字段劃分到不同表** | 不同 oftable 負責不同字段 | ❌ 錯誤 |
| **功能劃分到不同表** | 不同 oftable 負責不同功能階段 | ✅ 正確 |
| **每張表可匹配所有字段** | oftable 的 classifier 支持所有字段 | ✅ 正確 |
| **每張表選擇需要的字段** | 根據功能只匹配必要字段 | ✅ 正確 |
| **Classifier 內部字段索引** | TSS 根據字段組合創建子表 | ✅ 正確 (內部優化) |
| **DT 需支持任意字段組合** | 不能假設字段預先劃分 | ✅ 正確 (設計要求) |

**核心結論**:
> oftable 是按 **功能階段** 劃分,不是按 **字段** 劃分。每張表的 classifier 可以匹配任意字段組合,只是根據功能需求選擇性地使用不同字段。

**對 DT 的啟示**:
> DT 必須是通用的分類器,能處理任意字段組合,不能假設某張表只用特定字段。每個 oftable 的 DT 實例都需要根據實際規則集動態構建。
