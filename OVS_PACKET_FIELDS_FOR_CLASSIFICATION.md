# OVS 封包分類欄位完整指南

## 目錄
1. [概述](#概述)
2. [欄位層次結構](#欄位層次結構)
3. [詳細欄位列表](#詳細欄位列表)
4. [DT Classifier 使用的欄位](#dt-classifier-使用的欄位)
5. [欄位選擇策略](#欄位選擇策略)
6. [實際應用場景](#實際應用場景)

---

## 概述

### 什麼是分類欄位？

OVS 使用封包的各個欄位來：
1. **匹配流表規則**（Flow Table Matching）
2. **生成 Megaflow**（Megaflow Generation）
3. **決策封包處理**（Packet Processing Decision）

### 欄位總數

```c
// include/openvswitch/meta-flow.h
enum mf_field_id {
    // 共 126+ 個欄位定義
    MFF_DP_HASH,       // 第一個
    ...
    MFF_N_IDS          // 總數
};
```

---

## 欄位層次結構

### 按 OSI 模型分層

```
┌─────────────────────────────────────────────┐
│              元數據層 (Metadata)             │
│  - dp_hash, recirc_id, packet_type          │
│  - metadata, in_port, conj_id               │
│  - ct_state, ct_zone, ct_mark, ct_label    │
│  - registers (reg0-reg15, xreg, xxreg)      │
│  - tunnel metadata (tun_id, tun_src, etc.)  │
└─────────────────────────────────────────────┘
         ↓
┌─────────────────────────────────────────────┐
│            L2 層 (Ethernet/VLAN)             │
│  - eth_src, eth_dst                         │
│  - eth_type                                 │
│  - vlan_tci, vlan_vid, vlan_pcp             │
│  - mpls_label, mpls_tc, mpls_ttl            │
└─────────────────────────────────────────────┘
         ↓
┌─────────────────────────────────────────────┐
│              L3 層 (IP)                      │
│  IPv4:                                      │
│  - nw_src, nw_dst (IPv4 source/dest)        │
│  - nw_proto, nw_tos, nw_ttl                 │
│  - nw_frag                                  │
│                                             │
│  IPv6:                                      │
│  - ipv6_src, ipv6_dst                       │
│  - ipv6_label                               │
│                                             │
│  ARP:                                       │
│  - arp_op, arp_spa, arp_tpa                 │
│  - arp_sha, arp_tha                         │
└─────────────────────────────────────────────┘
         ↓
┌─────────────────────────────────────────────┐
│              L4 層 (Transport)               │
│  TCP/UDP/SCTP:                              │
│  - tp_src, tp_dst (port numbers)            │
│  - tcp_flags                                │
│                                             │
│  ICMP:                                      │
│  - icmp_type, icmp_code                     │
│                                             │
│  ICMPv6:                                    │
│  - icmpv6_type, icmpv6_code                 │
│  - nd_target, nd_sll, nd_tll                │
└─────────────────────────────────────────────┘
         ↓
┌─────────────────────────────────────────────┐
│           應用層相關 (L7+)                   │
│  - nsh_* (Network Service Header)           │
│  - igmp_group_ip4                           │
└─────────────────────────────────────────────┘
```

---

## 詳細欄位列表

### 📊 元數據欄位（Metadata）

| 欄位 | 類型 | 大小 | 說明 | 用途 |
|------|------|------|------|------|
| `dp_hash` | be32 | 4B | Datapath 計算的哈希值 | 內部優化 |
| `recirc_id` | be32 | 4B | 重循環 ID（0=初始） | 複雜處理流程 |
| `packet_type` | be32 | 4B | OpenFlow 1.5+ 封包類型 | 封包類型識別 |
| `conj_id` | be32 | 4B | Conjunction 動作 ID | 複雜匹配邏輯 |
| `metadata` | be64 | 8B | OpenFlow 元數據欄位 | 跨表傳遞信息 |
| `in_port` | be16/be32 | 2/4B | 輸入端口號 | **核心分類欄位** |
| `in_port_oxm` | be32 | 4B | OpenFlow 1.1+ 端口 | 端口匹配 |
| `actset_output` | be32 | 4B | Action Set 輸出端口 | 動作集處理 |
| `skb_priority` | be32 | 4B | 封包優先級（QoS） | QoS 排隊 |
| `pkt_mark` | be32 | 4B | 封包標記 | 系統間交互 |

#### Connection Tracking (CT) 欄位

| 欄位 | 類型 | 大小 | 說明 |
|------|------|------|------|
| `ct_state` | be32 | 4B | 連接追蹤狀態 |
| `ct_zone` | be16 | 2B | CT 區域 |
| `ct_mark` | be32 | 4B | CT 標記 |
| `ct_label` | be128 | 16B | CT 標籤 |
| `ct_nw_proto` | u8 | 1B | CT 原始 tuple 協議 |
| `ct_nw_src` | be32 | 4B | CT 原始 tuple IPv4 源 |
| `ct_nw_dst` | be32 | 4B | CT 原始 tuple IPv4 目標 |
| `ct_ipv6_src` | be128 | 16B | CT 原始 tuple IPv6 源 |
| `ct_ipv6_dst` | be128 | 16B | CT 原始 tuple IPv6 目標 |
| `ct_tp_src` | be16 | 2B | CT 原始 tuple 源端口 |
| `ct_tp_dst` | be16 | 2B | CT 原始 tuple 目標端口 |

#### 寄存器（Registers）

```c
// 可編程寄存器，用於存儲臨時數據
reg0 - reg15    // 16 個 32 位寄存器 (be32)
xreg0 - xreg7   // 8 個 64 位擴展寄存器 (be64)
xxreg0 - xxreg3 // 4 個 128 位超擴展寄存器 (be128)
```

#### 隧道元數據（Tunnel Metadata）

| 欄位 | 類型 | 大小 | 說明 |
|------|------|------|------|
| `tun_id` | be64 | 8B | 隧道 ID/VNI/Key |
| `tun_src` | be32 | 4B | 隧道 IPv4 源地址 |
| `tun_dst` | be32 | 4B | 隧道 IPv4 目標地址 |
| `tun_ipv6_src` | be128 | 16B | 隧道 IPv6 源地址 |
| `tun_ipv6_dst` | be128 | 16B | 隧道 IPv6 目標地址 |
| `tun_flags` | be16 | 2B | 隧道標誌 (df, csum, key, oam) |
| `tun_ttl` | u8 | 1B | 隧道 TTL |
| `tun_tos` | u8 | 1B | 隧道 ToS |
| `tun_gbp_id` | be16 | 2B | VXLAN Group-Based Policy ID |
| `tun_gbp_flags` | u8 | 1B | VXLAN GBP Flags |
| `tun_erspan_*` | varies | varies | ERSPAN 隧道欄位 |
| `tun_gtpu_*` | varies | varies | GTP-U 隧道欄位 |
| `tun_metadata0-63` | tunnelMD | 可變 | 通用隧道元數據 (最多 64 個) |

---

### 🔗 L2 層欄位（Ethernet）

| 欄位 | 類型 | 大小 | 說明 | 常用程度 |
|------|------|------|------|---------|
| `eth_src` | MAC | 6B | Ethernet 源 MAC 地址 | ⭐⭐⭐⭐ |
| `eth_dst` | MAC | 6B | Ethernet 目標 MAC 地址 | ⭐⭐⭐⭐⭐ |
| `eth_type` | be16 | 2B | Ethernet 類型/協議 | ⭐⭐⭐⭐⭐ |

**代碼定義**（`include/openvswitch/flow.h:120-126`）：
```c
struct flow {
    ...
    /* L2, Order the same as in the Ethernet header! */
    struct eth_addr dl_dst;     /* Ethernet destination address. */
    struct eth_addr dl_src;     /* Ethernet source address. */
    ovs_be16 dl_type;           /* Ethernet frame type. */
    ...
};
```

---

### 🏷️ VLAN 欄位

| 欄位 | 類型 | 大小 | 說明 | 常用程度 |
|------|------|------|------|---------|
| `vlan_tci` | be16 | 2B | 802.1Q TCI (包含 VID + PCP + CFI) | ⭐⭐⭐⭐ |
| `vlan_vid` | be16 | 2B | VLAN ID (OpenFlow 1.2+) | ⭐⭐⭐⭐ |
| `vlan_pcp` | u8 | 1B | VLAN 優先級代碼點 | ⭐⭐⭐ |

**支持多 VLAN**：
```c
#define FLOW_MAX_VLAN_HEADERS 2  // 最多 2 層 VLAN
union flow_vlan_hdr vlans[FLOW_MAX_VLAN_HEADERS];
```

---

### 📦 MPLS 欄位

| 欄位 | 類型 | 說明 |
|------|------|------|
| `mpls_label` | be32 | MPLS 標籤 (20 位) |
| `mpls_tc` | u8 | MPLS 流量類別 (3 位) |
| `mpls_bos` | u8 | MPLS 堆棧底部標誌 (1 位) |
| `mpls_ttl` | u8 | MPLS TTL (8 位) |

```c
#define FLOW_MAX_MPLS_LABELS 3  // 最多 3 層 MPLS
ovs_be32 mpls_lse[FLOW_MAX_MPLS_LABELS];
```

---

### 🌐 L3 層欄位（IP）

#### IPv4 欄位

| 欄位 | 類型 | 大小 | 說明 | 常用程度 |
|------|------|------|------|---------|
| `nw_src` | be32 | 4B | IPv4 源地址 | ⭐⭐⭐⭐⭐ |
| `nw_dst` | be32 | 4B | IPv4 目標地址 | ⭐⭐⭐⭐⭐ |
| `nw_proto` | u8 | 1B | IP 協議號 (TCP=6, UDP=17, ICMP=1) | ⭐⭐⭐⭐⭐ |
| `nw_tos` | u8 | 1B | IP ToS (包含 DSCP + ECN) | ⭐⭐⭐ |
| `nw_ttl` | u8 | 1B | IP TTL | ⭐⭐ |
| `nw_frag` | u8 | 1B | IP 分片標誌 | ⭐⭐ |

**代碼定義**（`include/openvswitch/flow.h:134-141`）：
```c
struct flow {
    ...
    /* L3 (64-bit aligned) */
    ovs_be32 nw_src;            /* IPv4 source address or ARP SPA. */
    ovs_be32 nw_dst;            /* IPv4 destination address or ARP TPA. */
    ...
    uint8_t nw_frag;            /* FLOW_FRAG_* flags. */
    uint8_t nw_tos;             /* IP ToS (including DSCP and ECN). */
    uint8_t nw_ttl;             /* IP TTL/Hop Limit. */
    uint8_t nw_proto;           /* IP protocol or low 8 bits of ARP opcode. */
};
```

#### IPv6 欄位

| 欄位 | 類型 | 大小 | 說明 | 常用程度 |
|------|------|------|------|---------|
| `ipv6_src` | IPv6 | 16B | IPv6 源地址 | ⭐⭐⭐⭐ |
| `ipv6_dst` | IPv6 | 16B | IPv6 目標地址 | ⭐⭐⭐⭐ |
| `ipv6_label` | be32 | 4B | IPv6 流標籤 | ⭐⭐ |
| `nw_proto` | u8 | 1B | IPv6 下一個標頭 | ⭐⭐⭐⭐ |
| `nw_tos` | u8 | 1B | IPv6 流量類別 | ⭐⭐⭐ |
| `nw_ttl` | u8 | 1B | IPv6 跳數限制 | ⭐⭐ |

```c
struct in6_addr ipv6_src;   /* IPv6 source address. */
struct in6_addr ipv6_dst;   /* IPv6 destination address. */
ovs_be32 ipv6_label;        /* IPv6 flow label. */
```

#### ARP 欄位

| 欄位 | 類型 | 說明 |
|------|------|------|
| `arp_op` | be16 | ARP 操作碼 (request=1, reply=2) |
| `arp_spa` | be32 | ARP 源協議地址（使用 nw_src） |
| `arp_tpa` | be32 | ARP 目標協議地址（使用 nw_dst） |
| `arp_sha` | MAC | ARP 源硬件地址 |
| `arp_tha` | MAC | ARP 目標硬件地址 |

---

### 🔌 L4 層欄位（Transport）

#### TCP/UDP/SCTP 欄位

| 欄位 | 類型 | 大小 | 說明 | 常用程度 |
|------|------|------|------|---------|
| `tp_src` | be16 | 2B | 源端口號 | ⭐⭐⭐⭐⭐ |
| `tp_dst` | be16 | 2B | 目標端口號 | ⭐⭐⭐⭐⭐ |
| `tcp_flags` | be16 | 2B | TCP 標誌 (SYN, ACK, FIN, etc.) | ⭐⭐⭐⭐ |

**代碼定義**（`include/openvswitch/flow.h:151-153`）：
```c
struct flow {
    ...
    /* L4 (64-bit aligned) */
    ovs_be16 tp_src;            /* TCP/UDP/SCTP source port/ICMP type. */
    ovs_be16 tp_dst;            /* TCP/UDP/SCTP destination port/ICMP code. */
    ...
};
```

#### ICMP 欄位

| 欄位 | 類型 | 說明 |
|------|------|------|
| `icmp_type` | u8 | ICMP 類型（使用 tp_src 高字節） |
| `icmp_code` | u8 | ICMP 代碼（使用 tp_dst 高字節） |

#### ICMPv6/ND 欄位

| 欄位 | 類型 | 說明 |
|------|------|------|
| `icmpv6_type` | u8 | ICMPv6 類型 |
| `icmpv6_code` | u8 | ICMPv6 代碼 |
| `nd_target` | IPv6 | ND 目標地址 |
| `nd_sll` | MAC | ND 源鏈路層地址 |
| `nd_tll` | MAC | ND 目標鏈路層地址 |

```c
struct in6_addr nd_target;  /* IPv6 neighbor discovery (ND) target. */
struct eth_addr arp_sha;    /* ARP/ND source hardware address. */
struct eth_addr arp_tha;    /* ARP/ND target hardware address. */
```

---

### 🎯 應用層相關欄位

#### Network Service Header (NSH)

```c
struct ovs_key_nsh nsh;     /* Network Service Header keys */
```

包含：
- `nsh_flags`, `nsh_mdtype`, `nsh_np`
- `nsh_spi` (Service Path Identifier)
- `nsh_si` (Service Index)
- `nsh_c1`, `nsh_c2`, `nsh_c3`, `nsh_c4` (Context Headers)

#### IGMP

| 欄位 | 類型 | 說明 |
|------|------|------|
| `igmp_group_ip4` | be32 | IGMP 組 IPv4 地址 |

---

## DT Classifier 使用的欄位

### 當前實現

**代碼位置**：`lib/dt-classifier.c:613-625`

```c
/* Select best field to split on based on entropy or information gain */
static const struct mf_field *
dt_select_split_field(struct rculist *rules, size_t n_rules)
{
    /* Common useful fields to try in order */
    static const enum mf_field_id candidate_fields[] = {
        MFF_IN_PORT,      // 輸入端口
        MFF_ETH_TYPE,     // Ethernet 類型
        MFF_IPV4_SRC,     // IPv4 源地址
        MFF_IPV4_DST,     // IPv4 目標地址
        MFF_IP_PROTO,     // IP 協議
        MFF_TCP_SRC,      // TCP 源端口
        MFF_TCP_DST,      // TCP 目標端口
        MFF_UDP_SRC,      // UDP 源端口
        MFF_UDP_DST,      // UDP 目標端口
    };
    
    /* Count how many rules care about each field */
    int field_counts[ARRAY_SIZE(candidate_fields)] = {0};
    
    // ... 選擇使用次數最多的欄位
}
```

### 為什麼選擇這些欄位？

| 欄位 | 理由 | 實際用途 |
|------|------|---------|
| **MFF_IN_PORT** | 最基本的分類依據 | 區分不同來源的流量 |
| **MFF_ETH_TYPE** | 協議識別關鍵 | 區分 IPv4/IPv6/ARP/VLAN |
| **MFF_IPV4_SRC** | 高區分度 | 源 IP 匹配，安全策略 |
| **MFF_IPV4_DST** | 高區分度 | 目標 IP 匹配，路由決策 |
| **MFF_IP_PROTO** | 傳輸層協議識別 | 區分 TCP/UDP/ICMP |
| **MFF_TCP_SRC** | 應用識別 | 端口匹配，負載均衡 |
| **MFF_TCP_DST** | 應用識別 | 服務端口匹配 |
| **MFF_UDP_SRC** | 應用識別 | UDP 流量分類 |
| **MFF_UDP_DST** | 應用識別 | DNS/DHCP 等服務 |

---

## 欄位選擇策略

### 1. 頻率法（當前 DT 實現）

```c
// 統計每個候選欄位在規則中的使用頻率
for (each candidate_field) {
    for (each rule) {
        if (rule uses this field && not wildcarded) {
            field_counts[field]++;
        }
    }
}

// 選擇使用次數最多的欄位
return field_with_max_count;
```

**優點**：
- ✅ 實現簡單
- ✅ 計算快速
- ✅ 反映實際規則分佈

**缺點**：
- ⚠️ 不考慮欄位值的分佈
- ⚠️ 可能不是最優分裂

### 2. 信息增益法（理想實現）

```
信息增益 = 分裂前熵 - 分裂後熵

熵(S) = -Σ p_i * log2(p_i)

其中：
- S 是規則集
- p_i 是第 i 類規則的比例
```

**優點**：
- ✅ 理論最優
- ✅ 產生最平衡的樹

**缺點**：
- ⚠️ 計算開銷大
- ⚠️ 實現複雜

### 3. 基於實際流量統計（未來改進）

```c
struct field_stats {
    enum mf_field_id field;
    uint64_t hit_count;        // 實際命中次數
    double selectivity;         // 選擇性（區分度）
    uint32_t unique_values;     // 唯一值數量
};

// 選擇高選擇性且高命中的欄位
score = hit_count * selectivity * log(unique_values);
```

---

## 實際應用場景

### 場景 1：數據中心流量分類

**典型規則集**：
```
Rule1: priority=100, in_port=1, ip_dst=10.0.1.0/24, action=forward(2)
Rule2: priority=100, in_port=1, ip_dst=10.0.2.0/24, action=forward(3)
Rule3: priority=90,  in_port=1, ip_proto=6, tp_dst=80, action=lb_group(1)
Rule4: priority=90,  in_port=1, ip_proto=6, tp_dst=443, action=lb_group(2)
Rule5: priority=80,  in_port=1, action=drop
```

**欄位使用統計**：
- `in_port`: 5 次 (100%)
- `ip_dst`: 2 次 (40%)
- `ip_proto`: 2 次 (40%)
- `tp_dst`: 2 次 (40%)

**DT 選擇**：`in_port`（最高頻率）

**但實際最佳**：`ip_dst`（區分度更高）

### 場景 2：防火牆規則

**典型規則集**：
```
Rule1: priority=100, ip_src=192.168.1.0/24, action=allow
Rule2: priority=100, ip_src=192.168.2.0/24, action=allow
Rule3: priority=100, ip_src=10.0.0.0/8, action=deny
Rule4: priority=90,  ip_proto=1, action=allow  (ICMP)
Rule5: priority=80,  action=drop
```

**欄位使用統計**：
- `ip_src`: 3 次 (60%) ← **最佳選擇**
- `ip_proto`: 1 次 (20%)

**DT 選擇**：`ip_src` ✅ 正確

### 場景 3：負載均衡

**典型規則集**：
```
Rule1: priority=100, ip_proto=6, tp_dst=80, ip_dst=203.0.113.10, action=lb1
Rule2: priority=100, ip_proto=6, tp_dst=80, ip_dst=203.0.113.11, action=lb2
Rule3: priority=100, ip_proto=6, tp_dst=443, ip_dst=203.0.113.10, action=lb3
Rule4: priority=100, ip_proto=6, tp_dst=443, ip_dst=203.0.113.11, action=lb4
```

**欄位使用統計**：
- `ip_proto`: 4 次 (100%)
- `tp_dst`: 4 次 (100%)
- `ip_dst`: 4 次 (100%)

**問題**：三個欄位頻率相同！

**解決**：需要考慮值的分佈
- `ip_proto`: 只有 1 個值（6=TCP）
- `tp_dst`: 2 個值（80, 443）← **較好**
- `ip_dst`: 2 個值（203.0.113.10, 203.0.113.11）← **較好**

**最佳策略**：先按 `tp_dst` 分（2 分支），再按 `ip_dst` 分（每分支 2 個葉節點）

---

## 欄位分類總結

### 核心分類欄位（必須支持）⭐⭐⭐⭐⭐

```
L2: eth_dst, eth_src, eth_type
L3: nw_src, nw_dst, nw_proto
L4: tp_src, tp_dst
Meta: in_port
```

### 常用分類欄位（應該支持）⭐⭐⭐⭐

```
L2: vlan_vid, vlan_pcp
L3: nw_tos, nw_ttl, ipv6_src, ipv6_dst
L4: tcp_flags
Meta: tun_id, metadata
```

### 高級分類欄位（可選支持）⭐⭐⭐

```
CT: ct_state, ct_zone, ct_mark
Tunnel: tun_src, tun_dst, tun_metadata*
MPLS: mpls_label, mpls_tc
```

### 特殊用途欄位（專用場景）⭐⭐

```
ARP: arp_op, arp_spa, arp_tha
ICMPv6: nd_target, nd_sll, nd_tll
NSH: nsh_spi, nsh_si
Registers: reg*, xreg*, xxreg*
```

---

## 完整欄位枚舉

### Meta-Flow 欄位 ID 定義

**文件**：`include/openvswitch/meta-flow.h:220-1800`

```c
enum mf_field_id {
    /* Metadata */
    MFF_DP_HASH,           // 0
    MFF_RECIRC_ID,         // 1
    MFF_PACKET_TYPE,       // 2
    MFF_CONJ_ID,           // 3
    MFF_TUN_ID,            // 4
    MFF_TUN_SRC,           // 5
    MFF_TUN_DST,           // 6
    MFF_TUN_IPV6_SRC,      // 7
    MFF_TUN_IPV6_DST,      // 8
    MFF_TUN_FLAGS,         // 9
    MFF_TUN_TTL,           // 10
    MFF_TUN_TOS,           // 11
    // ... (省略隧道元數據 12-75)
    MFF_TUN_METADATA0-63,  // 12-75
    MFF_METADATA,          // 76
    MFF_IN_PORT,           // 77 ⭐
    MFF_IN_PORT_OXM,       // 78
    MFF_ACTSET_OUTPUT,     // 79
    MFF_SKB_PRIORITY,      // 80
    MFF_PKT_MARK,          // 81
    MFF_CT_STATE,          // 82
    MFF_CT_ZONE,           // 83
    MFF_CT_MARK,           // 84
    MFF_CT_LABEL,          // 85
    MFF_CT_NW_PROTO,       // 86
    MFF_CT_NW_SRC,         // 87
    MFF_CT_NW_DST,         // 88
    MFF_CT_IPV6_SRC,       // 89
    MFF_CT_IPV6_DST,       // 90
    MFF_CT_TP_SRC,         // 91
    MFF_CT_TP_DST,         // 92
    MFF_REG0-15,           // 93-108 (16 registers)
    MFF_XREG0-7,           // 109-116 (8 extended registers)
    MFF_XXREG0-3,          // 117-120 (4 super registers)
    
    /* Ethernet */
    MFF_ETH_SRC,           // 121 ⭐
    MFF_ETH_DST,           // 122 ⭐
    MFF_ETH_TYPE,          // 123 ⭐
    
    /* VLAN */
    MFF_VLAN_TCI,          // 124
    MFF_DL_VLAN,           // 125
    MFF_VLAN_VID,          // 126
    MFF_VLAN_PCP,          // 127
    
    /* MPLS */
    MFF_MPLS_LABEL,        // 128
    MFF_MPLS_TC,           // 129
    MFF_MPLS_BOS,          // 130
    MFF_MPLS_TTL,          // 131
    
    /* IPv4 */
    MFF_IPV4_SRC,          // 132 ⭐ (aka MFF_NW_SRC)
    MFF_IPV4_DST,          // 133 ⭐ (aka MFF_NW_DST)
    
    /* IPv6 */
    MFF_IPV6_SRC,          // 134
    MFF_IPV6_DST,          // 135
    MFF_IPV6_LABEL,        // 136
    
    /* IP Common */
    MFF_IP_PROTO,          // 137 ⭐ (aka MFF_NW_PROTO)
    MFF_IP_DSCP,           // 138
    MFF_IP_ECN,            // 139
    MFF_IP_TTL,            // 140 (aka MFF_NW_TTL)
    MFF_IP_FRAG,           // 141 (aka MFF_NW_FRAG)
    
    /* ARP */
    MFF_ARP_OP,            // 142
    MFF_ARP_SPA,           // 143
    MFF_ARP_TPA,           // 144
    MFF_ARP_SHA,           // 145
    MFF_ARP_THA,           // 146
    
    /* TCP */
    MFF_TCP_SRC,           // 147 ⭐
    MFF_TCP_DST,           // 148 ⭐
    MFF_TCP_FLAGS,         // 149
    
    /* UDP */
    MFF_UDP_SRC,           // 150 ⭐
    MFF_UDP_DST,           // 151 ⭐
    
    /* SCTP */
    MFF_SCTP_SRC,          // 152
    MFF_SCTP_DST,          // 153
    
    /* ICMP */
    MFF_ICMP_TYPE,         // 154
    MFF_ICMP_CODE,         // 155
    
    /* ICMPv6 */
    MFF_ICMPV6_TYPE,       // 156
    MFF_ICMPV6_CODE,       // 157
    
    /* ND */
    MFF_ND_TARGET,         // 158
    MFF_ND_SLL,            // 159
    MFF_ND_TLL,            // 160
    MFF_ND_RESERVED,       // 161
    MFF_ND_OPTIONS_TYPE,   // 162
    
    /* NSH */
    MFF_NSH_FLAGS,         // 163
    MFF_NSH_MDTYPE,        // 164
    MFF_NSH_NP,            // 165
    MFF_NSH_SPI,           // 166
    MFF_NSH_SI,            // 167
    MFF_NSH_C1,            // 168
    MFF_NSH_C2,            // 169
    MFF_NSH_C3,            // 170
    MFF_NSH_C4,            // 171
    
    /* IGMP */
    MFF_IGMP_GROUP_IP4,    // 172
    
    MFF_N_IDS              // 總數：173
};
```

---

## 推薦閱讀

1. **官方文檔**：
   - `ovs-fields(7)` - 完整欄位列表和說明
   - `Documentation/topics/datapath.rst` - Datapath 設計

2. **代碼位置**：
   - `include/openvswitch/meta-flow.h` - 欄位定義
   - `include/openvswitch/flow.h` - Flow 結構
   - `lib/meta-flow.c` - 欄位操作函數
   - `lib/flow.c` - Flow 處理邏輯

3. **相關文檔**：
   - `DT_DECISION_RULES.md` - 決策規則機制
   - `DT_ALGORITHM_EXPLAINED.md` - DT 演算法詳解
   - `MEGAFLOW_UNIQUENESS_EXPLAINED.md` - Megaflow 唯一性

---

## 總結

### 關鍵要點

1. **OVS 支持 173 個分類欄位**
   - 涵蓋 L2-L4 所有常用欄位
   - 支持隧道、MPLS、NSH 等高級特性
   - 可擴展的寄存器和元數據

2. **DT Classifier 當前使用 9 個核心欄位**
   - in_port, eth_type
   - ipv4_src, ipv4_dst, ip_proto
   - tcp_src, tcp_dst, udp_src, udp_dst

3. **欄位選擇策略影響性能**
   - 頻率法：簡單但次優
   - 信息增益：最優但開銷大
   - 實際流量統計：平衡方案

4. **不同場景需要不同欄位**
   - 數據中心：IP 地址、端口
   - 防火牆：源 IP、協議
   - 負載均衡：目標 IP、端口
   - 隧道：tun_id、tun_src/dst

### 未來改進方向

1. **擴展候選欄位**
   ```c
   // 添加更多常用欄位
   MFF_VLAN_VID,
   MFF_IPV6_SRC,
   MFF_IPV6_DST,
   MFF_TUN_ID,
   ```

2. **智能欄位選擇**
   - 實現信息增益算法
   - 考慮欄位值分佈
   - 動態調整候選欄位

3. **基於流量學習**
   - 統計實際流量特徵
   - 自適應選擇最佳欄位
   - 支持用戶自定義策略
