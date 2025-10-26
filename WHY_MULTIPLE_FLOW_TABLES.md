# 為什麼 OpenFlow 交換器需要多張流表？

## 核心概念

**多表流水線 (Multi-Table Pipeline)** 是 OpenFlow 的核心設計特性，允許將複雜的數據包處理邏輯分解為多個階段。

```
數據包流程:
┌─────┐   ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌─────┐
│Input├──→│ Table 0 ├──→│ Table 1 ├──→│ Table 2 ├──→│ Table N ├──→│Output│
└─────┘   └─────────┘   └─────────┘   └─────────┘   └─────────┘   └─────┘
          準入控制      VLAN處理      MAC學習       轉發決策      輸出處理
```

---

## 為什麼需要多張表？

### **1. 模塊化設計 (Modularity)**

將不同功能分離到不同的表，每張表負責一個特定階段：

```
Table 0: 準入控制 (Admission Control)
  - 丟棄無效數據包 (如多播源地址)
  - 丟棄保留協議 (如 STP: 01:80:c2:xx:xx:xx)
  - 基本安全檢查

Table 1: VLAN 輸入處理
  - trunk 端口 → 保持 VLAN 標籤
  - access 端口 → 添加 VLAN 標籤

Table 2: MAC 學習
  - 學習源 MAC 地址 → 端口映射
  - 使用 learn() 動作自動添加流表項

Table 3: MAC 查找
  - 根據目標 MAC + VLAN 查找出口端口
  - 未知目標 → 泛洪到同 VLAN 所有端口

Table 4: 輸出處理
  - trunk 端口 → 保持 VLAN 標籤
  - access 端口 → 移除 VLAN 標籤
```

**優點**:
- 每張表職責單一,易於理解和維護
- 可以獨立修改某個階段而不影響其他階段
- 控制器邏輯更清晰

---

### **2. 可組合性 (Composability)**

不同功能可以通過表的組合實現，而不需要為每種組合寫獨立的規則：

#### 示例：VLAN + MAC 學習 + ACL

```
如果只有一張表:
  需要為每個 (VLAN, MAC, ACL規則) 組合寫一條流表項
  100 VLANs × 1000 MACs × 50 ACL規則 = 5,000,000 條流表項！

使用多張表:
  Table 0: 50 條 ACL 規則
  Table 1: 100 條 VLAN 規則  
  Table 2: 1000 條 MAC 學習規則
  Table 3: 1000 條 MAC 轉發規則
  總共: 2150 條流表項 (減少 99.96%!)
```

**關鍵機制**: `resubmit(,table_id)` 和 `goto_table:N`
```
actions=resubmit(,1)  // 重新提交到表 1 (OVS 擴展)
actions=goto_table:2  // 跳到表 2 (OpenFlow 標準)
```

---

### **3. 避免規則爆炸 (Rule Explosion Prevention)**

#### 場景：企業網絡

```
需求:
  - 100 個 VLAN
  - 每個 VLAN 1000 個 MAC 地址
  - 10 條 QoS 策略
  - 5 條安全策略

單表方案:
  100 × 1000 × 10 × 5 = 5,000,000 條規則 (不可行)

多表方案 (5 張表):
  Table 0: 安全策略      →    5 條規則
  Table 1: VLAN 處理     →  100 條規則
  Table 2: MAC 學習      → 1000 條規則 (動態學習)
  Table 3: QoS 分類      →   10 條規則
  Table 4: 轉發決策      → 1000 條規則
  總計: 2115 條規則 (減少 99.96%)
```

---

### **4. 支持高級功能**

#### (A) 遞歸重提交 (Resubmit)

允許在同一表或之前的表中重新查找：

```c
// ofproto-dpif-xlate.c 限制:
#define MAX_DEPTH 64        // 最大遞歸深度
#define MAX_RESUBMITS 4096  // 最大重提交次數

// 用途: 實現循環、條件分支
```

**示例: 負載均衡**
```
Table 10: 選擇後端服務器
  - 使用 hash 選擇服務器 A/B/C
  - resubmit(,20) → 應用服務器特定規則

Table 20: 服務器規則
  - 修改目標 IP 和 MAC
  - resubmit(,30) → 執行轉發
```

#### (B) 條件執行 (Conditional Execution)

```
Table 1: 檢查是否是已知 MAC
  - 如果已知 → goto_table:3 (直接轉發)
  - 如果未知 → goto_table:2 (學習 + 泛洪)

Table 2: MAC 學習
  - 使用 learn() 動作添加到 Table 3
  - 泛洪到所有端口
  - goto_table:4

Table 3: 已知 MAC 轉發
  - 單播到特定端口
  - goto_table:4

Table 4: 輸出處理
```

#### (C) 狀態機實現

```
Table 0: 初始狀態
  → 根據包類型跳到不同表

Table 10-19: TCP 狀態機
  - Table 10: SYN 處理
  - Table 11: SYN-ACK 處理
  - Table 12: ACK 處理
  - Table 13: FIN 處理

Table 20-29: 防火牆規則

Table 30+: 轉發決策
```

---

### **5. 性能優化**

#### (A) 早期丟棄 (Early Drop)

```
Table 0: 丟棄無效包 (1000 pps)
  ↓ 只有有效包進入後續表
Table 1: VLAN 處理 (900 pps)
  ↓
Table 2: MAC 查找 (900 pps)

vs 單表方案:
  所有包必須經過所有規則檢查 (1000 pps 全程)
```

#### (B) 緩存效率

```
不同表可以使用不同的匹配字段:
  Table 0: 只匹配 dl_src, dl_dst (小型分類器)
  Table 1: 只匹配 dl_vlan, in_port (小型分類器)
  Table 2: 只匹配 dl_dst (小型分類器)

vs 單表:
  需要同時匹配所有字段 (大型分類器,緩存命中率低)
```

#### (C) 並行處理潛力

未來可以並行執行獨立的表 (目前 OVS 是串行):
```
        ┌─ Table 2A (ACL) ─┐
Pkt → ──┤                  ├─→ Merge → Table 3
        └─ Table 2B (QoS) ─┘
```

---

### **6. 實際案例：OVS 高級教程**

OVS 官方教程實現的 VLAN 交換機：

```python
# 端口配置
p1: trunk (所有 VLAN)
p2: access VLAN 20
p3: access VLAN 30
p4: access VLAN 30

# 5 張表的流水線
Table 0: 準入控制
  priority=100,dl_src=01:00:00:00:00:00/01:00:00:00:00:00,actions=drop
  priority=100,dl_dst=01:80:c2:00:00:00/ff:ff:ff:ff:ff:f0,actions=drop
  priority=0,actions=resubmit(,1)

Table 1: VLAN 輸入處理
  in_port=1,actions=resubmit(,2)                    # trunk: 保持
  in_port=2,actions=mod_vlan_vid:20,resubmit(,2)   # access: 打標籤
  in_port=3,actions=mod_vlan_vid:30,resubmit(,2)
  in_port=4,actions=mod_vlan_vid:30,resubmit(,2)

Table 2: MAC 學習
  priority=0,actions=learn(table=10,                # 動態學習到 Table 10
                           NXM_OF_VLAN_TCI[0..11],
                           NXM_OF_ETH_DST[]=NXM_OF_ETH_SRC[],
                           load:NXM_OF_IN_PORT[]->NXM_NX_REG0[0..15]),
                    resubmit(,3)

Table 3 (實際是 Table 10): MAC 查找
  # 由 learn() 動態添加,格式如:
  vlan=20,dl_dst=00:11:22:33:44:55,actions=load:2->NXM_NX_REG0[0..15]

Table 4: 輸出處理
  vlan=20,reg0=1,actions=output:1                   # trunk: 保持標籤
  vlan=20,reg0=2,actions=strip_vlan,output:2        # access: 移除標籤
  vlan=30,reg0=1,actions=output:1
  vlan=30,reg0=3,actions=strip_vlan,output:3
  vlan=30,reg0=4,actions=strip_vlan,output:4
```

**如果只用一張表**: 需要為每個 (源端口, 源MAC, VLAN, 目標MAC, 目標端口) 組合寫規則 → 幾乎不可能！

---

## OpenFlow 規範支持

### OpenFlow 1.0 (2009)
- 只支持單表 (Table 0)
- 所有邏輯必須塞在一張表

### OpenFlow 1.1+ (2011-)
- 支持 **254 張表** (0-253, 表 255 保留)
- 引入 `goto_table` 指令
- 強制順序: 只能跳到 **更高編號** 的表 (防止無限循環)

### OVS 擴展
- `resubmit(port,table)` 動作
  - 可以跳到 **任意表** (包括當前或之前的表)
  - 有深度限制防止無限循環 (MAX_DEPTH=64)
- `learn()` 動作: 動態添加流表項
- `conjunction()`: 高效實現多維匹配

---

## 代碼中的體現

### 檢查表 ID 合法性
```c
// ofproto/ofproto.c
static bool
check_table_id(const struct ofproto *ofproto, uint8_t table_id)
{
    return table_id == OFPTT_ALL || table_id < ofproto->n_tables;
}
```

### 表遍歷宏
```c
// 遍歷所有表
#define OFPROTO_FOR_EACH_TABLE(TABLE, OFPROTO)              \
    for ((TABLE) = (OFPROTO)->tables;                       \
         (TABLE) < &(OFPROTO)->tables[(OFPROTO)->n_tables]; \
         (TABLE)++)

// 遍歷匹配的表 (支持 OFPTT_ALL=0xff)
#define FOR_EACH_MATCHING_TABLE(TABLE, TABLE_ID, OFPROTO)
```

### 重提交深度限制
```c
// ofproto/ofproto-dpif-xlate.c
#define MAX_DEPTH 64        // 遞歸深度限制
#define MAX_RESUBMITS 4096  // 總重提交次數限制

struct xlate_ctx {
    int depth;              // 當前深度
    int resubmits;          // 總重提交次數
};
```

---

## DT 集成的影響

**好消息**: DT 集成 **不需要關心多表邏輯**！

```
多表邏輯在 ofproto 層處理:
  ofproto → tables[0..N] → oftable → classifier

DT 只需要替換 classifier:
  oftable {
      struct classifier cls;  ← 這裡可以是 TSS 或 DT
  }
```

### 每張表都有獨立的分類器

```c
struct ofproto {
    struct oftable *tables;  // 表數組
    int n_tables;            // 表數量 (通常 254)
};

struct oftable {
    struct classifier cls;   // 每張表有自己的分類器實例
};

// 數據包查找時:
for (table_id = 0; table_id < n_tables; table_id++) {
    rule = classifier_lookup(&tables[table_id].cls, &flow);
    if (rule) {
        execute_actions(rule->actions);
        if (actions包含 goto_table:N) {
            table_id = N - 1;  // 跳到表 N (循環會 +1)
            continue;
        }
        break;
    }
}
```

### DT 的工作範圍

```
DT 只負責單張表內的查找:
  Input:  flow (數據包字段)
  Output: rule (匹配的規則)

不需要處理:
  ✗ 表間跳轉 (由 ofproto 處理)
  ✗ resubmit 邏輯 (由 xlate 層處理)
  ✗ goto_table 指令 (由 actions 執行器處理)
```

---

## 實際數據

### OVS 默認配置
```bash
$ ovs-ofctl dump-tables br0
OFPST_TABLE reply (xid=0x2):
  table 0: active=10, lookup=1000, matched=950
  table 1: active=5,  lookup=950,  matched=900
  table 2: active=100, lookup=900, matched=850
  # ... 最多 254 張表
```

### 典型表數量
- **簡單橋接**: 1 張表
- **VLAN 交換機**: 5 張表
- **企業網關**: 10-20 張表
- **SDN 控制器 (如 Faucet)**: 10-15 張表
- **安全網關**: 20-30 張表
- **最大值**: 254 張表 (OpenFlow 限制)

---

## 總結

| 原因 | 說明 | 效果 |
|------|------|------|
| **模塊化** | 每張表一個功能 | 易維護、易理解 |
| **可組合性** | 功能通過表組合 | 避免規則爆炸 |
| **性能** | 早期丟棄、小分類器 | 提升查找速度 |
| **靈活性** | 條件跳轉、狀態機 | 支持複雜邏輯 |
| **擴展性** | 動態添加功能 | 無需重寫全部規則 |

**核心思想**: 
> "Divide and Conquer" - 將複雜的單片邏輯分解為多個簡單階段

**對 DT 集成的啟示**:
> DT 只需專注於 **單表查找性能**，多表邏輯已經由 OVS 架構完美處理！

---

## 參考資料

- OpenFlow 規範 1.5: Section 5.1 "Pipeline Processing"
- OVS 高級教程: `Documentation/tutorials/ovs-advanced.rst`
- OVS 源碼: `ofproto/ofproto.c` 的表管理邏輯
- Faucet SDN 控制器: 實際使用 9 張表實現 L2/L3 交換
