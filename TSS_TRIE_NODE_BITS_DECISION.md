# TSS Trie 节点位数决策机制详解

## 核心问题

**问题**: TSS 的 trie 中，每个节点要处理多少位 (n_bits) 是如何决定的？

**答案**: **由插入过程动态决定，基于前缀之间的公共部分和差异点**。

---

## 目录

1. [决策原则](#决策原则)
2. [三种决策场景](#三种决策场景)
3. [核心算法详解](#核心算法详解)
4. [完整示例](#完整示例)
5. [算法特性](#算法特性)
6. [与传统二叉树的对比](#与传统二叉树的对比)

---

## 决策原则

### 基本规则

```
节点的 n_bits 由以下因素决定:
  1. ⭐ 与现有节点的公共前缀长度
  2. ⭐ 需要表示的剩余前缀长度
  3. ⭐ TRIE_PREFIX_BITS 限制 (最多 32 bits)

核心思想:
  - 尽可能合并公共前缀到一个节点
  - 在第一个差异位分支
  - 最小化树的深度
```

### 决策时机

```
创建新节点时:
  trie_branch_create(prefix, ofs, plen, n_rules)
  
  参数:
    - prefix: 前缀值
    - ofs: 已经处理的位数
    - plen: 还需要处理的位数 ⭐ 决定 n_bits
    - n_rules: 规则数量
```

---

## 三种决策场景

### 场景 1: 首次插入（空树）

```c
// 代码位置: trie_insert_prefix() 末尾
if (!node) {
    // 到达空边，创建新分支
    ovsrcu_set(edge, trie_branch_create(prefix, ofs, mlen - ofs, 1));
}
```

#### 决策逻辑

```
trie_branch_create(prefix, ofs, plen, n_rules):
  
  if (plen <= TRIE_PREFIX_BITS) {  // plen <= 32
      node->n_bits = plen;  ⭐ 直接使用全部剩余长度
  } else {
      // plen > 32，需要拆分成多个节点
      node->n_bits = TRIE_PREFIX_BITS;  // 32
      创建子节点处理剩余的 (plen - 32) 位
  }
```

#### 示例

```
插入第一个前缀: 10.0.0.0/8

调用:
  trie_branch_create(10.0.0.0, ofs=0, plen=8, n_rules=1)

决策:
  plen = 8 <= 32
  → node->n_bits = 8  ⭐ 一个节点存储全部 8 位

结果:
    [10/8]
    n_bits = 8
```

---

### 场景 2: 完全匹配路径 + 扩展

```c
// 代码位置: trie_insert_prefix() 中间
for (; node; edge = trie_next_edge(node, prefix, ofs)) {
    eqbits = trie_prefix_equal_bits(node, prefix, ofs, mlen);
    ofs += eqbits;
    
    if (eqbits == node->n_bits) {  // ⭐ 完全匹配
        if (ofs == mlen) {
            node->n_rules++;  // 完全相同的前缀
            return;
        }
        // ofs < mlen，继续向下
        continue;
    }
}

// 到达空边，创建新节点
ovsrcu_set(edge, trie_branch_create(prefix, ofs, mlen - ofs, 1));
```

#### 决策逻辑

```
当前节点完全匹配，需要继续向下:
  - ofs 累加当前节点的 n_bits
  - 如果到达空边，创建新节点
  - 新节点的 n_bits = mlen - ofs ⭐ 剩余的全部位数
```

#### 示例

```
已有: [10/8]
插入: 10.0.0.0/16

过程:
  1. 节点 [10/8], ofs=0
     eqbits = 8 (完全匹配)
     ofs = 0 + 8 = 8
     ofs < mlen (8 < 16)，继续向下
  
  2. edges[0] 为空，创建新节点
     trie_branch_create(10.0.0.0, ofs=8, plen=16-8=8, n_rules=1)
     
     决策: plen = 8
     → node->n_bits = 8  ⭐ 剩余的 8 位

结果:
    [10/8]
      |
    [0/8]
    (10.0.0.0 的第 9-16 位)
```

---

### 场景 3: 部分匹配（需要分裂）⭐ 最关键

```c
// 代码位置: trie_insert_prefix()
for (; node; ...) {
    eqbits = trie_prefix_equal_bits(node, prefix, ofs, mlen);
    ofs += eqbits;
    
    if (eqbits < node->n_bits) {  // ⭐ 部分匹配，需要分裂
        // 创建新父节点，n_bits = eqbits ⭐
        new_parent = trie_branch_create(prefix, ofs - eqbits, eqbits,
                                        ofs == mlen ? 1 : 0);
        
        // 调整旧节点
        node->prefix <<= eqbits;
        node->n_bits -= eqbits;  ⭐
        
        // 如果新前缀更长，创建另一个分支
        if (ofs < mlen) {
            trie_branch_create(prefix, ofs, mlen - ofs, 1);
        }
    }
}
```

#### 决策逻辑

```
当前节点部分匹配:
  1. 计算公共前缀长度: eqbits
  2. 创建新父节点: n_bits = eqbits ⭐ 只包含公共部分
  3. 调整旧节点: n_bits -= eqbits ⭐ 减去公共部分
  4. 创建新分支: n_bits = mlen - ofs ⭐ 剩余的全部
```

#### 示例：关键场景

```
已有: [10.0/16]
      n_bits = 16
      prefix = 0x0A000000 (10.0.0.0)

插入: 10.1.0.0/16

比较:
  10.0.0.0 = 00001010 00000000 xxxxxxxx xxxxxxxx
  10.1.0.0 = 00001010 00000001 xxxxxxxx xxxxxxxx
             ^^^^^^^^ ^^^^^^^
             8 bits   7 bits  差异
  
  公共前缀: 前 15 位 (eqbits = 15)

分裂过程:
  1. ⭐ 创建新父节点 (公共部分)
     trie_branch_create(10.0.0.0, ofs=0, plen=15, n_rules=0)
     → n_bits = 15  ⭐ 恰好是公共前缀长度
  
  2. ⭐ 调整旧节点 (差异部分)
     旧节点原来: n_bits = 16
     减去公共: n_bits = 16 - 15 = 1  ⭐
     prefix <<= 15 (左移去掉公共部分)
  
  3. ⭐ 创建新分支 (另一个差异部分)
     trie_branch_create(10.1.0.0, ofs=15, plen=16-15=1, n_rules=1)
     → n_bits = 1  ⭐

结果:
        [10.0/15]      公共前缀，n_bits=15
         /     \
    [0/1]     [1/1]    差异部分，各 n_bits=1
    10.0.x.x  10.1.x.x
```

---

## 核心算法详解

### 函数 1: `trie_prefix_equal_bits()` - 计算公共前缀

```c
static unsigned int
trie_prefix_equal_bits(const struct trie_node *node, const ovs_be32 prefix[],
                       unsigned int ofs, unsigned int plen)
{
    // 比较 node->prefix 和新 prefix 的公共位数
    return prefix_equal_bits(node->prefix, 
                             MIN(node->n_bits, plen - ofs),
                             prefix, ofs);
}
```

#### 工作原理

```c
static unsigned int
prefix_equal_bits(uint32_t prefix, unsigned int n_bits, 
                  const ovs_be32 value[], unsigned int ofs)
{
    // 1. 异或得到差异位
    uint64_t diff = prefix ^ raw_get_prefix(value, ofs, n_bits);
    
    // 2. 计算前导零（即公共前缀长度）
    return raw_clz64(diff << 32 | UINT64_C(1) << (63 - n_bits));
}
```

#### 示例

```
节点: prefix = 0x0A00 (1010000000000000), n_bits = 16
新值: 10.1.0.0 (从 ofs=0 开始)
      = 0x0A01 (1010000000000001)

计算:
  diff = 0x0A00 ^ 0x0A01 = 0x0001
       = 0000000000000001
  
  前导零数: 15
  → eqbits = 15  ⭐ 前 15 位相同
```

---

### 函数 2: `trie_branch_create()` - 创建节点

```c
static struct trie_node *
trie_branch_create(const ovs_be32 *prefix, unsigned int ofs, 
                   unsigned int plen, unsigned int n_rules)
{
    struct trie_node *node = xmalloc(sizeof *node);

    node->prefix = trie_get_prefix(prefix, ofs, plen);

    if (plen <= TRIE_PREFIX_BITS) {  // 32
        // ⭐ 情况 1: 前缀长度 <= 32，一个节点搞定
        node->n_bits = plen;
        ovsrcu_set_hidden(&node->edges[0], NULL);
        ovsrcu_set_hidden(&node->edges[1], NULL);
        node->n_rules = n_rules;
    } else {
        // ⭐ 情况 2: 前缀长度 > 32，需要多个节点
        // 当前节点存储前 32 位
        node->n_bits = TRIE_PREFIX_BITS;  // 32
        
        // 递归创建子节点存储剩余位
        struct trie_node *subnode = trie_branch_create(
            prefix,
            ofs + TRIE_PREFIX_BITS,      // 跳过前 32 位
            plen - TRIE_PREFIX_BITS,     // 剩余位数
            n_rules
        );
        
        int bit = get_bit_at(subnode->prefix, 0);
        ovsrcu_set_hidden(&node->edges[bit], subnode);
        ovsrcu_set_hidden(&node->edges[!bit], NULL);
        node->n_rules = 0;
    }
    return node;
}
```

#### 决策表

| plen | 决策 | n_bits | 说明 |
|------|------|--------|------|
| 1-32 | 单节点 | plen | 直接使用 |
| 33-64 | 2个节点 | 32, plen-32 | 拆分 |
| 65-96 | 3个节点 | 32, 32, plen-64 | 递归拆分 |
| > 96 | 多个节点 | 32, 32, ... | 继续递归 |

---

## 完整示例

### 场景：构建复杂 Trie

#### 插入顺序

```
规则 1: 10.0.0.0/8
规则 2: 10.0.0.0/16
规则 3: 10.1.0.0/16
规则 4: 10.0.128.0/17
规则 5: 192.168.0.0/16
```

#### 详细构建过程

##### 步骤 1: 插入 10.0.0.0/8

```
调用: trie_branch_create(10.0.0.0, ofs=0, plen=8, n_rules=1)

决策:
  plen = 8 <= 32
  → n_bits = 8

结果:
    [10/8]
```

##### 步骤 2: 插入 10.0.0.0/16

```
遍历:
  节点 [10/8]:
    eqbits = 8 (完全匹配)
    ofs = 8
    ofs < mlen (8 < 16)，继续
  
  到达空边，创建新节点:
    trie_branch_create(10.0.0.0, ofs=8, plen=8, n_rules=1)
    
决策:
  plen = 8
  → n_bits = 8

结果:
    [10/8]
      |
    [0/8]
```

##### 步骤 3: 插入 10.1.0.0/16

```
遍历:
  节点 [10/8]:
    eqbits = 8
    ofs = 8
  
  节点 [0/8] (10.0.x.x):
    比较: 10.0.0.0 vs 10.1.0.0 (从 ofs=8 开始)
    第 9 位: 0 vs 0 (相同)
    第 10-16 位: 0000000 vs 0000001
    eqbits = 7 (只有 7 位相同)
    ofs = 8 + 7 = 15
    
    ⭐ 部分匹配，需要分裂！

分裂:
  1. 创建新父节点 (公共部分):
     trie_branch_create(..., ofs=8, plen=7, n_rules=0)
     → n_bits = 7  ⭐
  
  2. 调整旧节点:
     原来: [0/8]
     现在: [0/1], n_bits = 8 - 7 = 1  ⭐
  
  3. 创建新分支:
     trie_branch_create(..., ofs=15, plen=1, n_rules=1)
     → n_bits = 1  ⭐

结果:
    [10/8]
      |
    [0/7]              ← 公共部分 (10.0/10.1 的第 9-15 位)
     / \
[0/1] [1/1]           ← 差异部分 (第 16 位)
10.0  10.1
```

##### 步骤 4: 插入 10.0.128.0/17

```
遍历:
  [10/8] → ofs=8
  [0/7] → ofs=15
  [0/1] → ofs=16
  
  到达空边，创建:
    trie_branch_create(..., ofs=16, plen=1, n_rules=1)

决策:
  plen = 1
  → n_bits = 1  ⭐

结果:
    [10/8]
      |
    [0/7]
     / \
[0/1] [1/1]
  |
[1/1]                 ← 10.0.128.0 的第 17 位
```

##### 步骤 5: 插入 192.168.0.0/16

```
遍历:
  节点 [10/8]:
    比较: 10.x.x.x vs 192.168.x.x
    第 1 位: 0 vs 1 (立即不匹配!)
    eqbits = 0
    ofs = 0
    
    ⭐ 没有公共前缀，需要在根分裂！

分裂:
  1. 创建空父节点:
     trie_branch_create(..., ofs=0, plen=0, n_rules=0)
     → n_bits = 0  ⭐ 空节点！（理论上，实际可能是 1）
  
  2. [10/8] 放到 edges[0]
  3. 创建 [192.168/16] 放到 edges[1]

结果:
         []
        /  \
    [10/8]  [192.168/16]
      |
    [0/7]
     / \
   ...
```

**注**: 实际实现中，根节点可能会被优化为第一个差异位。

---

## 算法特性

### 1. 自适应性

```
✅ 根据实际前缀自动调整节点大小
✅ 公共前缀长 → 节点 n_bits 大
✅ 差异早 → 分裂早，节点 n_bits 小
```

### 2. 空间优化

```
比较:
  固定 1 位/节点: 10.0.0.0/24 需要 24 个节点
  自适应: 10.0.0.0/24 可能只需要 3 个节点 (8+8+8)

空间节省: 87.5%
```

### 3. 时间优化

```
比较:
  固定 1 位/节点: 查找深度 = 前缀长度 (最多 32)
  自适应: 查找深度 = 节点数 (通常 1-4)

查找加速: 8-32 倍
```

### 4. 动态平衡

```
插入不同的前缀组合 → 不同的树结构
但总是最优的（最少节点数）

示例:
  只有 /8 前缀 → 所有节点 n_bits=8
  只有 /32 前缀 → 节点 n_bits 各不相同（根据差异位置）
```

---

## 与传统二叉树的对比

### 传统二叉 Trie (Radix Tree with 1 bit/level)

```
规则: 每层固定处理 1 位

示例: 10.0.0.0/8
    []
    └─[0]          bit 1
       └─[0]       bit 2
          └─[0]    bit 3
             └─[0] bit 4
                └─[1] bit 5
                   └─[0] bit 6
                      └─[1] bit 7
                         └─[0] bit 8 ← 规则在这

深度: 8
节点数: 8
```

### OVS Trie (可变长度节点)

```
规则: 每层动态决定处理多少位

示例: 10.0.0.0/8
    [10/8]         bits 1-8

深度: 1
节点数: 1

压缩率: 8:1
```

### 性能对比

| 特性 | 传统二叉树 | OVS Trie | 优势 |
|------|-----------|----------|------|
| 节点数 (10个/8前缀) | 80 | 10 | 8倍 |
| 深度 (/24) | 24 | 3 | 8倍 |
| 查找时间 | O(32) | O(4) | 8倍 |
| 空间占用 | 高 | 低 | 75-90% |
| 分支因子 | 2 | 2 | 相同 |
| 实现复杂度 | 简单 | 中等 | - |

---

## 关键代码片段总结

### 决策点 1: 创建节点

```c
trie_branch_create(prefix, ofs, plen, n_rules) {
    node->n_bits = (plen <= 32) ? plen : 32;  ⭐ 关键决策
}
```

### 决策点 2: 分裂节点

```c
eqbits = trie_prefix_equal_bits(node, prefix, ofs, mlen);  ⭐ 计算公共位

if (eqbits < node->n_bits) {
    // 父节点: n_bits = eqbits       ⭐ 公共部分
    // 旧节点: n_bits -= eqbits      ⭐ 剩余部分
    // 新节点: n_bits = mlen - ofs   ⭐ 新分支
}
```

### 决策点 3: 扩展节点

```c
if (eqbits == node->n_bits) {
    ofs += eqbits;  ⭐ 累加已处理位数
    继续向下;
}

// 到达空边
trie_branch_create(prefix, ofs, mlen - ofs, 1);  ⭐ 剩余全部
```

---

## 总结

### 核心答案

```
❓ 如何决定每层处理几个 bit？

✅ 答案：动态决定，基于三个原则

1. 首次创建: n_bits = 剩余长度 (最多 32)
2. 完全匹配: 继续向下，新节点 n_bits = 剩余长度
3. 部分匹配: 
   - 父节点 n_bits = 公共长度
   - 子节点 n_bits = 差异长度
```

### 算法优势

```
1. ✅ 自适应: 根据实际前缀自动优化
2. ✅ 空间高效: 比固定 1 位/层节省 75-90%
3. ✅ 时间高效: 查找深度减少 8-32 倍
4. ✅ 动态平衡: 插入顺序不影响最终结构的优化程度
```

### 对 DT 的启示

```
DT 可以借鉴这个思想:
  1. 不要固定每个节点检查一个字段
  2. 可以在一个节点检查多个相关字段
  3. 根据字段值的分布动态调整树结构
  4. 合并"无歧义"的检查步骤
```

---

**文档创建时间**: 2025-10-19  
**作者**: GitHub Copilot  
**版本**: 1.0  
**相关文档**: 
- TSS_TRIE_CONSTRUCTION.md
- TSS_CLASSIFICATION_MECHANISM.md
