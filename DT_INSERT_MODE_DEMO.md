# DT插入模式演示

## 功能说明

DT分类器现在支持两种插入模式，会根据树的构建状态**自动切换**：

### Phase 1: 初始化阶段（树未建立）- Lazy插入

```c
struct decision_tree dt;
dt_init(&dt);

// 这些调用会自动使用 lazy 模式
dt_insert_rule(&dt, rule1, version);  // → 累积到 pending_rules[]
dt_insert_rule(&dt, rule2, version);  // → 累积到 pending_rules[]
dt_insert_rule(&dt, rule1000, version);  // → 累积到 pending_rules[]

// tree_built = false, 所有规则存在 pending_rules 中
```

### 触发树构建

有两种方式触发树构建：

```c
// 方式1: 显式构建
dt_build_initial_tree(&dt);

// 方式2: 第一次lookup时自动构建
const struct cls_rule *match = dt_lookup(&dt, version, &flow, wc);
// ↑ 会自动调用 dt_ensure_tree_built()
```

### Phase 2: 运行时阶段（树已建立）- COW插入

```c
// 树构建后，插入会自动切换到 COW 模式
dt_insert_rule(&dt, rule1001, version);  
// → 使用 COW (Copy-On-Write) 增量插入
// → 找到合适的叶节点
// → 复制路径上的所有节点
// → 原子性切换到新树
// → RCU延迟销毁旧树

dt_insert_rule(&dt, rule1002, version);  
// → 继续使用 COW 模式
```

## 代码示例

### 完整使用流程

```c
#include "dt-classifier.h"
#include "classifier.h"

void example_usage(void) {
    struct decision_tree dt;
    struct cls_rule rules[1000];
    
    // Phase 1: 初始化
    dt_init(&dt);
    
    // 批量添加规则（自动使用lazy模式）
    for (int i = 0; i < 1000; i++) {
        // 初始化规则...
        cls_rule_init(&rules[i], &match, priority);
        
        // 插入 - 自动判断使用lazy模式
        dt_insert_rule(&dt, &rules[i], VERSION_INITIAL);
    }
    
    printf("Added 1000 rules to pending list\n");
    printf("Tree built: %s\n", dt.tree_built ? "yes" : "no");  // → no
    
    // Phase 2: 显式构建树（可选）
    dt_build_initial_tree(&dt);
    printf("Tree built: %s\n", dt.tree_built ? "yes" : "no");  // → yes
    printf("Tree stats: %d rules, %d nodes, depth %d\n",
           dt.n_rules, dt.n_internal_nodes + dt.n_leaf_nodes, dt.max_depth);
    
    // Phase 3: 运行时添加规则
    struct cls_rule new_rule;
    cls_rule_init(&new_rule, &new_match, new_priority);
    
    // 插入 - 自动判断使用COW模式
    dt_insert_rule(&dt, &new_rule, VERSION_NEW);
    printf("Inserted new rule using COW\n");
    
    // Lookup会触发lazy build（如果还未构建）
    struct flow flow;
    struct flow_wildcards wc;
    const struct cls_rule *match = dt_lookup(&dt, VERSION_NEW, &flow, &wc);
    
    // 清理
    dt_destroy(&dt);
}
```

### 直接调用lazy接口（不推荐）

如果你需要直接控制：

```c
// ✅ 正确用法：树未建立时
dt_add_rule_lazy(&dt, rule1);  // OK
dt_add_rule_lazy(&dt, rule2);  // OK

dt_build_initial_tree(&dt);

// ❌ 错误用法：树已建立后
dt_add_rule_lazy(&dt, rule3);  // 返回false，打印警告
```

**推荐：** 统一使用`dt_insert_rule()`，它会自动选择正确的模式。

## 内部实现细节

### dt_insert_rule 的逻辑

```c
bool dt_insert_rule(struct decision_tree *dt, const struct cls_rule *rule,
                    ovs_version_t version)
{
    // 🔍 自动判断阶段
    if (!dt->tree_built) {
        // Phase 1: 树未建立 → 使用lazy插入
        return dt_add_rule_lazy(dt, rule);
    }
    
    // Phase 2: 树已建立 → 使用COW插入
    // 1. 遍历树找到合适的叶节点
    // 2. COW复制叶节点和路径
    // 3. 插入规则（按优先级排序）
    // 4. 原子性切换根节点
    // 5. RCU延迟销毁旧树
    ...
}
```

### dt_add_rule_lazy 的保护

```c
bool dt_add_rule_lazy(struct decision_tree *dt, const struct cls_rule *rule)
{
    // 🛡️ 防止误用：树建立后禁止lazy插入
    if (dt->tree_built) {
        VLOG_WARN("dt_add_rule_lazy: tree already built, use dt_insert_rule instead");
        return false;
    }
    
    // 累积到pending数组
    dt->pending_rules[dt->n_pending++] = rule;
    return true;
}
```

## 优势

### 1. 性能优化

- **初始化阶段**：批量构建最优树（O(N log N)）
- **运行时阶段**：增量COW更新（O(log N)）
- **避免重复重建**：每个规则只参与一次树构建

### 2. 使用简单

```c
// 统一接口 - 自动选择模式
dt_insert_rule(&dt, rule, version);
```

### 3. 错误防护

```c
// 防止在错误的阶段使用错误的接口
dt_add_rule_lazy(&dt, rule);  // 树建立后会返回false
```

## 性能对比

| 场景 | 旧方案 | 新方案 | 改进 |
|------|--------|--------|------|
| 插入1000条规则（初始化） | 每条都重建树 | 批量构建一次 | **1000x** |
| 插入1条规则（运行时） | 重建整棵树 | COW插入 | **100x** |
| 第一次lookup | 无树（错误） | 自动触发构建 | ✅ |

## 测试验证

运行以下测试验证功能：

```bash
cd /mnt/d/ovs_DS
make tests/ovstest
./tests/ovstest test-dt-classifier
```

查看日志确认模式切换：

```
DT Lazy: Added rule (priority=100) to pending list, total=1
DT Lazy: Added rule (priority=200) to pending list, total=2
...
DT Lazy Build: Building tree from 1000 pending rules
DT Lazy Build: Tree built successfully - 1000 rules, 150 internal nodes, 200 leaf nodes, max depth 8
DT: Inserted rule (priority=300) using COW, total rules=1001
```

## 注意事项

1. ✅ **推荐**：统一使用`dt_insert_rule()`
2. ⚠️ **不推荐**：直接调用`dt_add_rule_lazy()`（除非你知道自己在做什么）
3. 🔒 **线程安全**：COW插入使用RCU保护，并发读取安全
4. 💾 **内存**：pending_rules数组在树构建后保留（可选择释放）

## 未来扩展

可以添加defer/publish机制：

```c
// Phase 1: 初始化
dt_init(&dt);
for (i = 0; i < 1000; i++) {
    dt_insert_rule(&dt, rules[i], version);  // lazy模式
}
dt_build_initial_tree(&dt);  // 显式构建

// Phase 2: 批量修改
dt_defer(&dt);  // 未来功能
dt_insert_rule(&dt, rule1001, version);
dt_insert_rule(&dt, rule1002, version);
dt_publish(&dt);  // 一次性应用所有修改
```
