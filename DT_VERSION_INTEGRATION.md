# DT 整合时的 Version 考虑

## 核心问题

**问题**: 在修改 classifier 整合 DT 时，是否需要考虑 version 机制？

**答案**: **✅ 绝对需要！Version 是 classifier 的核心功能，必须完整支持**。

---

## 目录

1. [为什么必须支持 Version](#为什么必须支持-version)
2. [Classifier API 中的 Version](#classifier-api-中的-version)
3. [DT 已有的 Version 支持](#dt-已有的-version-支持)
4. [整合时的 Version 处理](#整合时的-version-处理)
5. [实现检查清单](#实现检查清单)
6. [测试验证](#测试验证)

---

## 为什么必须支持 Version

### OVS 的设计要求

```c
// lib/classifier.h 注释明确说明：

/*
 * Classifier Versioning
 * =====================
 *
 * Classifier lookups are always done in a specific classifier version, where
 * a version is defined to be a natural number.
 *
 * When a new rule is added to a classifier, it is set to become visible in a
 * specific version.  If the version number used at insert time is larger than
 * any version number currently used in lookups, the new rule is said to be
 * invisible to lookups.
 *
 * Similarly, a rule can be marked as to be deleted in a future version.
 */
```

**关键**: 所有 classifier 操作都使用 version！

### Classifier 的核心 API

```c
// lib/classifier.h 

// ⭐ 插入规则 - 需要 version
void classifier_insert(struct classifier *, 
                       const struct cls_rule *,
                       ovs_version_t version,           // ⭐ 必需
                       const struct cls_conjunction *,
                       size_t n_conjunctions);

// ⭐ 替换规则 - 需要 version
const struct cls_rule *classifier_replace(struct classifier *,
                                          const struct cls_rule *,
                                          ovs_version_t version,  // ⭐ 必需
                                          const struct cls_conjunction *,
                                          size_t n_conjunctions);

// ⭐ 查找规则 - 需要 version
const struct cls_rule *classifier_lookup(const struct classifier *,
                                         ovs_version_t version,   // ⭐ 必需
                                         struct flow *,
                                         struct flow_wildcards *wc);

// ⭐ 标记规则不可见 - 需要 version
void cls_rule_make_invisible_in_version(const struct cls_rule *,
                                        ovs_version_t version);  // ⭐ 必需
```

**结论**: 如果 DT 后端不支持 version，**无法通过 classifier API 调用**！

---

## Classifier API 中的 Version

### 版本化操作的语义

#### 1. 插入规则

```c
// 场景: 添加新规则到版本 100
classifier_insert(cls, rule, version=100, ...);

行为:
  - 规则的 add_version = 100
  - 规则的 remove_version = OVS_VERSION_NOT_REMOVED
  - 版本 < 100 的查找: 看不到这条规则 ❌
  - 版本 >= 100 的查找: 可以看到这条规则 ✅

用途:
  - 批量更新规则（所有新规则用同一版本号）
  - 事务性修改（新规则在提交前不可见）
```

#### 2. 查找规则

```c
// 场景: 在版本 50 查找
const struct cls_rule *rule = classifier_lookup(cls, version=50, flow, wc);

行为:
  - 只返回在版本 50 可见的规则
  - 检查每条匹配规则: add_version <= 50 < remove_version
  - 跳过所有不可见的规则

重要性:
  - 实现 MVCC (多版本并发控制)
  - 读者看到一致的快照
  - 不同版本的读者可以并发访问
```

#### 3. 删除规则

```c
// 场景: 标记规则在版本 100 删除
cls_rule_make_invisible_in_version(rule, version=100);

行为:
  - 设置 remove_version = 100
  - 版本 < 100 的查找: 仍能看到规则 ✅
  - 版本 >= 100 的查找: 看不到规则 ❌

用途:
  - 平滑删除（旧读者继续看到规则）
  - 延迟清理（等所有旧读者完成）
```

---

## DT 已有的 Version 支持

### 好消息：DT 已经实现了基础 Version 支持！

#### DT 头文件

```c
// lib/dt-classifier.h

#include "versions.h"  // ⭐ 已经包含 version 头文件

// ⭐ 查找函数已支持 version
const struct cls_rule *
dt_lookup(const struct decision_tree *dt, 
          ovs_version_t version,      // ⭐ 已有 version 参数
          const struct flow *flow, 
          struct flow_wildcards *wc);

// ⭐ 插入函数已支持 version
bool dt_insert_rule(struct decision_tree *dt, 
                    const struct cls_rule *rule,
                    ovs_version_t version);  // ⭐ 已有 version 参数
```

### 当前实现状态

检查 `lib/dt-classifier.c`:

```c
// ✅ dt_lookup() 接受 version 参数
const struct cls_rule *
dt_lookup(const struct decision_tree *dt, ovs_version_t version,
          const struct flow *flow, struct flow_wildcards *wc)
{
    // ⚠️ 需要检查: 是否实际使用了 version？
}

// ✅ dt_insert_rule() 接受 version 参数
bool dt_insert_rule(struct decision_tree *dt, const struct cls_rule *rule,
                    ovs_version_t version)
{
    // ⚠️ 需要检查: 是否正确设置规则的 version？
}
```

---

## 整合时的 Version 处理

### 方案 A: Classifier 层统一处理 (推荐)

```c
// lib/classifier.c

void
classifier_insert(struct classifier *cls, const struct cls_rule *rule,
                  ovs_version_t version,  // ⭐ 从 API 接收 version
                  const struct cls_conjunction *conjs, size_t n_conjs)
{
    // ⭐ 关键: 在 classifier 层设置规则的 version
    struct cls_match *cls_match = cls_match_alloc(rule, version, conjs, n_conjs);
    //                                                   ^^^^^^^
    //                                                   设置 add_version
    
    // 分发到后端
    if (cls->backend_type == BACKEND_TSS) {
        // TSS 后端 - 规则已经有正确的 version
        tss_insert(cls, cls_match);
    } else if (cls->backend_type == BACKEND_DT) {
        // DT 后端 - 规则已经有正确的 version
        dt_insert_rule(&cls->dt, rule, version);
        //                              ^^^^^^^ 
        //                              传递 version (可能不需要，因为规则已设置)
    }
}

const struct cls_rule *
classifier_lookup(const struct classifier *cls, ovs_version_t version,
                  struct flow *flow, struct flow_wildcards *wc)
{
    if (cls->backend_type == BACKEND_TSS) {
        return tss_lookup(cls, version, flow, wc);
        //                     ^^^^^^^ 传递给 TSS
    } else if (cls->backend_type == BACKEND_DT) {
        return dt_lookup(&cls->dt, version, flow, wc);
        //                          ^^^^^^^ 传递给 DT
    }
}
```

**优势**:
- ✅ Version 处理在 classifier 层统一
- ✅ 后端只需遵守 version 语义
- ✅ TSS 和 DT 行为一致

### 方案 B: 后端独立处理

```c
// ❌ 不推荐：让每个后端自己管理 version

void
classifier_insert(struct classifier *cls, const struct cls_rule *rule,
                  ovs_version_t version, ...)
{
    if (cls->backend_type == BACKEND_TSS) {
        // TSS 自己设置 version
        tss_insert_with_version(cls, rule, version);
    } else if (cls->backend_type == BACKEND_DT) {
        // DT 自己设置 version
        dt_insert_with_version(&cls->dt, rule, version);
    }
}

问题:
  - ❌ 逻辑重复
  - ❌ 容易不一致
  - ❌ 增加复杂度
```

---

## 实现检查清单

### 阶段 1: 确认 cls_rule 已有 version 支持

```c
// lib/classifier-private.h

struct cls_match {
    // ...
    struct versions versions;  // ⭐ 确认这个字段存在
    // ...
};

// ✅ 确认 cls_match_alloc() 设置 version
static struct cls_match *
cls_match_alloc(const struct cls_rule *rule, ovs_version_t version, ...)
{
    // ...
    cls_match->versions = VERSIONS_INITIALIZER(version, version);
    // ...
}

// ✅ 确认可见性检查函数存在
static inline bool
cls_match_visible_in_version(const struct cls_match *match,
                             ovs_version_t version)
{
    return versions_visible_in_version(&match->versions, version);
}
```

### 阶段 2: DT 后端使用 version

```c
// lib/dt-classifier.c

const struct cls_rule *
dt_lookup(const struct decision_tree *dt, ovs_version_t version,
          const struct flow *flow, struct flow_wildcards *wc)
{
    struct dt_node *node = ovsrcu_get(struct dt_node *, &dt->root);
    
    while (node) {
        if (node->type == DT_NODE_LEAF) {
            // ⭐ 关键: 检查规则可见性
            const struct cls_rule *rule;
            RCULIST_FOR_EACH (rule, node, &node->leaf.rules) {
                struct cls_match *match = get_cls_match(rule);
                
                // ⭐ 必须检查 version！
                if (cls_match_visible_in_version(match, version)) {
                    if (miniflow_matches_flow(&match->flow, flow)) {
                        // 找到匹配且可见的规则
                        return rule;
                    }
                }
            }
            return NULL;  // 没有可见的匹配规则
        }
        
        // 内部节点 - 继续遍历
        node = next_node(node, flow);
    }
    
    return NULL;
}
```

### 阶段 3: Classifier 分发层传递 version

```c
// lib/classifier.c

void
classifier_insert(struct classifier *cls, const struct cls_rule *rule,
                  ovs_version_t version,
                  const struct cls_conjunction *conjs, size_t n_conjs)
{
    // 创建 cls_match (这里设置 version)
    struct cls_match *cls_match = cls_match_alloc(rule, version, conjs, n_conjs);
    
    // 分发到后端
    if (use_tss(cls)) {
        // TSS 后端
        tss_insert_internal(cls, cls_match);
    } else {
        // DT 后端 - 规则的 cls_match 已有正确的 version
        dt_insert_rule(&cls->dt, rule, version);
    }
}

const struct cls_rule *
classifier_lookup(const struct classifier *cls, ovs_version_t version,
                  struct flow *flow, struct flow_wildcards *wc)
{
    if (use_tss(cls)) {
        return tss_lookup(cls, version, flow, wc);
    } else {
        return dt_lookup(&cls->dt, version, flow, wc);  // ⭐ 传递 version
    }
}

void
cls_rule_make_invisible_in_version(const struct cls_rule *rule,
                                   ovs_version_t version)
{
    struct cls_match *cls_match = get_cls_match_protected(rule);
    
    // ⭐ 设置 remove_version (后端无关)
    cls_match_set_remove_version(cls_match, version);
    
    // 不需要通知后端 - 后端在查找时会检查可见性
}
```

---

## 整合步骤中的 Version 检查

### Step 1: 修改 classifier.h 结构

```c
// lib/classifier.h

struct classifier {
    // ... 现有字段
    
    // ⭐ 添加后端类型
    enum classifier_backend {
        BACKEND_TSS,
        BACKEND_DT
    } backend_type;
    
    // ⭐ 后端联合体
    union {
        struct {
            // TSS 相关字段
            // ... 
        } tss;
        
        struct decision_tree dt;  // DT 后端
    };
};

// ✅ API 函数签名不需要改变 - 已经有 version 参数！
void classifier_insert(struct classifier *, const struct cls_rule *,
                       ovs_version_t version,  // ⭐ 已有
                       const struct cls_conjunction *,
                       size_t n_conjunctions);

const struct cls_rule *classifier_lookup(const struct classifier *,
                                         ovs_version_t version,  // ⭐ 已有
                                         struct flow *,
                                         struct flow_wildcards *wc);
```

**Version 检查**: ✅ API 已经支持 version，无需修改！

### Step 2: 实现分发逻辑

```c
// lib/classifier.c

void
classifier_insert(struct classifier *cls, const struct cls_rule *rule,
                  ovs_version_t version,
                  const struct cls_conjunction *conjs, size_t n_conjs)
{
    // ⭐ Version 处理 - 在分发前完成
    struct cls_match *cls_match = cls_match_alloc(rule, version, conjs, n_conjs);
    
    if (cls->backend_type == BACKEND_TSS) {
        // TSS 路径 (现有代码)
        // ... 现有 TSS 插入逻辑
    } else {
        // DT 路径 (新增)
        dt_insert_rule(&cls->dt, rule, version);
        //                              ^^^^^^^ 传递 version
    }
}

const struct cls_rule *
classifier_lookup(const struct classifier *cls, ovs_version_t version,
                  struct flow *flow, struct flow_wildcards *wc)
{
    if (cls->backend_type == BACKEND_TSS) {
        // TSS 路径
        return classifier_lookup__(cls, version, flow, wc, ...);
        //                              ^^^^^^^ 传递 version
    } else {
        // DT 路径
        return dt_lookup(&cls->dt, version, flow, wc);
        //                          ^^^^^^^ 传递 version
    }
}
```

**Version 检查**: ✅ 确保两个后端都接收并使用 version！

### Step 3: 测试 Version 行为

```c
// tests/test-classifier.c (或新的测试文件)

static void
test_dt_versioning(void)
{
    struct classifier cls;
    struct cls_rule rule1, rule2;
    
    classifier_init(&cls, flow_segment_u64s);
    // 假设有方法切换到 DT 后端
    classifier_set_backend(&cls, BACKEND_DT);
    
    // 测试 1: 规则在未来版本可见
    cls_rule_init(&rule1, &match, 100);
    classifier_insert(&cls, &rule1, version=10, NULL, 0);
    //                               ^^^^^^^^^^
    //                               规则在 v10 添加
    
    // 查找 v5 - 应该看不到
    assert(classifier_lookup(&cls, version=5, flow, wc) == NULL);
    
    // 查找 v10 - 应该看到
    assert(classifier_lookup(&cls, version=10, flow, wc) == &rule1);
    
    // 查找 v20 - 应该看到
    assert(classifier_lookup(&cls, version=20, flow, wc) == &rule1);
    
    // 测试 2: 标记删除
    cls_rule_make_invisible_in_version(&rule1, version=15);
    //                                         ^^^^^^^^^^^
    //                                         v15 删除
    
    // 查找 v10 - 应该还能看到
    assert(classifier_lookup(&cls, version=10, flow, wc) == &rule1);
    
    // 查找 v15 - 应该看不到
    assert(classifier_lookup(&cls, version=15, flow, wc) == NULL);
    
    // 测试 3: 规则替换
    cls_rule_init(&rule2, &match, 100);
    classifier_replace(&cls, &rule2, version=20, NULL, 0);
    //                               ^^^^^^^^^^^
    //                               新规则 v20 可见
    
    // 查找 v15 - 看到 rule1 (如果恢复可见性)
    // 查找 v20 - 看到 rule2
    
    classifier_destroy(&cls);
}
```

**Version 检查**: ✅ 测试所有版本化操作的语义！

---

## 实现检查清单

### 必须实现的 Version 支持

- [ ] **DT lookup 检查 version**
  ```c
  const struct cls_rule *dt_lookup(..., ovs_version_t version, ...)
  {
      // ⭐ 必须: 检查每条规则的可见性
      if (cls_match_visible_in_version(match, version)) {
          return rule;
      }
  }
  ```

- [ ] **DT insert 接收 version**
  ```c
  bool dt_insert_rule(..., ovs_version_t version)
  {
      // ⭐ 注意: 规则的 cls_match 已由 classifier 层设置 version
      // DT 可能不需要直接使用 version 参数
      // 但应该保留接口一致性
  }
  ```

- [ ] **Classifier 分发传递 version**
  ```c
  // ⭐ 所有 classifier API 必须传递 version 到后端
  classifier_insert(cls, rule, version, ...) 
    → dt_insert_rule(dt, rule, version)
  
  classifier_lookup(cls, version, ...)
    → dt_lookup(dt, version, ...)
  ```

- [ ] **Version 语义测试**
  ```c
  // ⭐ 测试:
  // 1. 规则在指定版本可见
  // 2. 规则在指定版本删除
  // 3. 多版本并发查找
  // 4. 事务性批量更新
  ```

### 可选的 Version 优化

- [ ] **版本化树结构 (高级)**
  ```c
  // 可选: 为不同版本维护不同的树结构
  // 类似 Git 的分支机制
  struct decision_tree {
      struct dt_version *versions;  // 版本链表
  };
  ```

- [ ] **延迟删除 (配合 RCU)**
  ```c
  // 可选: 在 grace period 后清理旧版本节点
  void dt_cleanup_old_versions(struct decision_tree *dt, 
                                ovs_version_t oldest_active_version);
  ```

---

## 测试验证

### 单元测试

```bash
# 测试 DT 的 version 支持
make tests/test-classifier
tests/test-classifier -v dt_versioning
```

### 集成测试

```c
// 验证 TSS 和 DT 的 version 行为一致

static void
test_backend_version_consistency(void)
{
    struct classifier cls_tss, cls_dt;
    
    // 初始化两个后端
    classifier_init(&cls_tss, flow_segment_u64s);
    classifier_init(&cls_dt, flow_segment_u64s);
    classifier_set_backend(&cls_dt, BACKEND_DT);
    
    // 添加相同规则
    classifier_insert(&cls_tss, &rule, version=10, NULL, 0);
    classifier_insert(&cls_dt, &rule, version=10, NULL, 0);
    
    // 相同版本查找 - 结果应一致
    const struct cls_rule *r_tss = classifier_lookup(&cls_tss, 5, flow, wc);
    const struct cls_rule *r_dt = classifier_lookup(&cls_dt, 5, flow, wc);
    assert((r_tss == NULL) == (r_dt == NULL));  // 两者都为 NULL 或都不为 NULL
    
    r_tss = classifier_lookup(&cls_tss, 10, flow, wc);
    r_dt = classifier_lookup(&cls_dt, 10, flow, wc);
    assert(r_tss != NULL && r_dt != NULL);  // 两者都能找到
    
    // 删除规则
    cls_rule_make_invisible_in_version(&rule, 15);
    
    r_tss = classifier_lookup(&cls_tss, 20, flow, wc);
    r_dt = classifier_lookup(&cls_dt, 20, flow, wc);
    assert(r_tss == NULL && r_dt == NULL);  // 两者都看不到
}
```

---

## 总结

### 核心要点

| 项目 | 状态 | 说明 |
|------|------|------|
| **API 支持** | ✅ 已有 | classifier API 已包含 version 参数 |
| **DT 接口** | ✅ 已有 | dt_lookup/dt_insert 已有 version 参数 |
| **实现需求** | ⚠️ 需检查 | 确保 DT 内部正确使用 version |
| **分发层** | 🔧 需实现 | classifier 分发时传递 version |
| **测试覆盖** | 🔧 需添加 | 版本化操作的测试用例 |

### 实现优先级

```
P0 (必须):
  1. ✅ DT lookup 检查规则可见性
  2. ✅ Classifier 分发传递 version
  3. ✅ 基础 version 语义测试

P1 (重要):
  4. ✅ 多版本并发测试
  5. ✅ TSS vs DT 一致性测试

P2 (可选):
  6. ⭕ 版本化树结构 (性能优化)
  7. ⭕ 延迟清理 (内存优化)
```

### 关键建议

```
1. ✅ Version 是必需功能，不是可选
2. ✅ 复用现有的 struct versions
3. ✅ 在 classifier 层统一管理 version
4. ✅ 后端只需遵守 version 可见性规则
5. ✅ 测试时确保 TSS 和 DT 行为一致
```

---

**文档创建时间**: 2025-10-19  
**作者**: GitHub Copilot  
**版本**: 1.0  
**相关文档**:
- OVS_VERSION_MECHANISM.md
- DT_INTEGRATION_DESIGN.md
- DT_NEXT_STEPS.md
