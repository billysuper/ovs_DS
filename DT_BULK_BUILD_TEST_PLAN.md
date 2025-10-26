# DT 全量建树测试方案

## 目标

**快速验证 DT 功能，避免复杂的 TSS 集成**

策略：独立测试程序 + 全量建树 → 简单、快速、有效

---

## 方案设计

### 策略：独立测试程序（推荐）⭐

```
优势:
  ✅ 完全独立于 OVS classifier
  ✅ 不需要修改 TSS 代码
  ✅ 可以自由控制测试流程
  ✅ 快速验证 DT 核心功能
  ✅ 30 分钟内完成

步骤:
  1. 创建独立测试程序
  2. 生成测试规则
  3. 用 dt_build_tree() 全量建树
  4. 测试查找功能
  5. 验证正确性
```

---

## 实现代码

### 步骤 1: 创建测试程序

```c
// tests/test-dt-bulk.c

#include <config.h>
#include "dt-classifier.h"
#include "classifier.h"
#include "classifier-private.h"
#include "flow.h"
#include "openvswitch/match.h"
#include "openvswitch/ofp-actions.h"
#include "openvswitch/vlog.h"
#include "ovstest.h"
#include "timeval.h"
#include "util.h"

VLOG_DEFINE_THIS_MODULE(test_dt_bulk);

/* 测试用的规则结构 */
struct test_rule {
    struct cls_rule cls_rule;
    int id;  /* 规则 ID，用于验证 */
};

/* 创建测试规则 */
static struct test_rule *
create_test_rule(int id, ovs_be32 nw_src, int priority)
{
    struct test_rule *rule = xzalloc(sizeof *rule);
    struct match match;
    
    match_init_catchall(&match);
    match_set_nw_src(&match, nw_src);
    
    cls_rule_init(&rule->cls_rule, &match, priority);
    rule->id = id;
    
    return rule;
}

/* 主测试函数 */
static void
test_dt_bulk_basic(void)
{
    struct decision_tree dt;
    struct rculist rules_list;
    struct test_rule *rules[100];
    int i;
    
    VLOG_INFO("=== Test: DT Bulk Build Basic ===");
    
    /* 1. 初始化 */
    dt_init(&dt);
    rculist_init(&rules_list);
    
    /* 2. 创建 100 条测试规则 */
    VLOG_INFO("Creating 100 test rules...");
    for (i = 0; i < 100; i++) {
        ovs_be32 ip = htonl(0x0a000000 + i);  /* 10.0.0.0 - 10.0.0.99 */
        rules[i] = create_test_rule(i, ip, 100 - i);  /* 优先级递减 */
        
        rculist_push_back(&rules_list, 
                          CONST_CAST(struct rculist *, &rules[i]->cls_rule.node));
    }
    
    /* 3. 全量建树 */
    VLOG_INFO("Building tree with dt_build_tree()...");
    long long start = time_msec();
    
    struct dt_node *root = dt_build_tree(&rules_list, 100, 10);
    
    long long end = time_msec();
    VLOG_INFO("Tree built in %lld ms", end - start);
    
    if (!root) {
        VLOG_ERR("Failed to build tree!");
        goto cleanup;
    }
    
    ovsrcu_set(&dt.root, root);
    dt.n_rules = 100;
    
    /* 4. 测试查找 */
    VLOG_INFO("Testing lookups...");
    int correct = 0;
    
    for (i = 0; i < 100; i++) {
        struct flow flow;
        memset(&flow, 0, sizeof flow);
        flow.nw_src = htonl(0x0a000000 + i);
        
        const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
        
        if (found) {
            struct test_rule *test_rule = 
                CONTAINER_OF(found, struct test_rule, cls_rule);
            
            if (test_rule->id == i) {
                correct++;
            } else {
                VLOG_WARN("Rule %d: expected id=%d, got id=%d", 
                          i, i, test_rule->id);
            }
        } else {
            VLOG_WARN("Rule %d: not found!", i);
        }
    }
    
    VLOG_INFO("Lookup test: %d/%d correct", correct, 100);
    
    /* 5. 获取统计信息 */
    size_t n_rules, n_internal, n_leaf, max_depth;
    dt_get_stats(&dt, &n_rules, &n_internal, &n_leaf, &max_depth);
    
    VLOG_INFO("Tree stats:");
    VLOG_INFO("  Rules: %zu", n_rules);
    VLOG_INFO("  Internal nodes: %zu", n_internal);
    VLOG_INFO("  Leaf nodes: %zu", n_leaf);
    VLOG_INFO("  Max depth: %zu", max_depth);
    
cleanup:
    /* 6. 清理 */
    dt_destroy(&dt);
    for (i = 0; i < 100; i++) {
        cls_rule_destroy(&rules[i]->cls_rule);
        free(rules[i]);
    }
    
    VLOG_INFO("=== Test completed ===\n");
}

/* 测试不同规模 */
static void
test_dt_bulk_scale(void)
{
    int sizes[] = {10, 50, 100, 500, 1000};
    int i, j;
    
    VLOG_INFO("=== Test: DT Bulk Build Scale ===");
    
    for (j = 0; j < ARRAY_SIZE(sizes); j++) {
        int n = sizes[j];
        struct decision_tree dt;
        struct rculist rules_list;
        struct test_rule **rules = xmalloc(n * sizeof *rules);
        
        dt_init(&dt);
        rculist_init(&rules_list);
        
        /* 创建规则 */
        for (i = 0; i < n; i++) {
            ovs_be32 ip = htonl(0x0a000000 + i);
            rules[i] = create_test_rule(i, ip, n - i);
            rculist_push_back(&rules_list, 
                              CONST_CAST(struct rculist *, &rules[i]->cls_rule.node));
        }
        
        /* 建树并计时 */
        long long start = time_msec();
        struct dt_node *root = dt_build_tree(&rules_list, n, 10);
        long long end = time_msec();
        
        if (root) {
            ovsrcu_set(&dt.root, root);
            dt.n_rules = n;
            
            VLOG_INFO("Size %d: built in %lld ms", n, end - start);
            
            size_t n_rules, n_internal, n_leaf, max_depth;
            dt_get_stats(&dt, &n_rules, &n_internal, &n_leaf, &max_depth);
            VLOG_INFO("  Internal: %zu, Leaf: %zu, Depth: %zu", 
                      n_internal, n_leaf, max_depth);
        }
        
        /* 清理 */
        dt_destroy(&dt);
        for (i = 0; i < n; i++) {
            cls_rule_destroy(&rules[i]->cls_rule);
            free(rules[i]);
        }
        free(rules);
    }
    
    VLOG_INFO("=== Scale test completed ===\n");
}

/* 测试查找性能 */
static void
test_dt_bulk_lookup_perf(void)
{
    struct decision_tree dt;
    struct rculist rules_list;
    struct test_rule *rules[1000];
    int i;
    
    VLOG_INFO("=== Test: DT Lookup Performance ===");
    
    /* 准备 */
    dt_init(&dt);
    rculist_init(&rules_list);
    
    for (i = 0; i < 1000; i++) {
        ovs_be32 ip = htonl(0x0a000000 + i);
        rules[i] = create_test_rule(i, ip, 1000 - i);
        rculist_push_back(&rules_list, 
                          CONST_CAST(struct rculist *, &rules[i]->cls_rule.node));
    }
    
    struct dt_node *root = dt_build_tree(&rules_list, 1000, 10);
    ovsrcu_set(&dt.root, root);
    dt.n_rules = 1000;
    
    /* 性能测试 */
    VLOG_INFO("Performing 10000 lookups...");
    long long start = time_msec();
    
    for (i = 0; i < 10000; i++) {
        struct flow flow;
        memset(&flow, 0, sizeof flow);
        flow.nw_src = htonl(0x0a000000 + (i % 1000));
        
        const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
        (void)found;  /* 忽略结果，只测性能 */
    }
    
    long long end = time_msec();
    long long total_us = (end - start) * 1000;
    
    VLOG_INFO("10000 lookups in %lld ms (avg %.2f us per lookup)", 
              end - start, (double)total_us / 10000);
    
    /* 清理 */
    dt_destroy(&dt);
    for (i = 0; i < 1000; i++) {
        cls_rule_destroy(&rules[i]->cls_rule);
        free(rules[i]);
    }
    
    VLOG_INFO("=== Performance test completed ===\n");
}

/* 主入口 */
static void
test_dt_bulk(int argc OVS_UNUSED, char *argv[] OVS_UNUSED)
{
    test_dt_bulk_basic();
    test_dt_bulk_scale();
    test_dt_bulk_lookup_perf();
}

OVSTEST_REGISTER("test-dt-bulk", test_dt_bulk);
```

---

### 步骤 2: 添加到构建系统

```makefile
# tests/automake.mk

# 在现有的 tests_ovstest_SOURCES 中添加:
tests_ovstest_SOURCES += \
    tests/test-dt-bulk.c

# 或者创建独立的测试程序:
noinst_PROGRAMS += tests/test-dt-bulk

tests_test_dt_bulk_SOURCES = tests/test-dt-bulk.c
tests_test_dt_bulk_LDADD = lib/libopenvswitch.la
```

---

### 步骤 3: 编译和运行

```bash
# PowerShell (Windows)

# 1. 清理
make clean

# 2. 编译
make tests/ovstest

# 3. 运行测试
./tests/ovstest test-dt-bulk

# 或者如果创建了独立程序:
make tests/test-dt-bulk
./tests/test-dt-bulk
```

---

## 预期输出

```
=== Test: DT Bulk Build Basic ===
Creating 100 test rules...
Building tree with dt_build_tree()...
Tree built in 2 ms
Testing lookups...
Lookup test: 100/100 correct
Tree stats:
  Rules: 100
  Internal nodes: 15
  Leaf nodes: 16
  Max depth: 8
=== Test completed ===

=== Test: DT Bulk Build Scale ===
Size 10: built in 0 ms
  Internal: 1, Leaf: 2, Depth: 2
Size 50: built in 1 ms
  Internal: 7, Leaf: 8, Depth: 5
Size 100: built in 2 ms
  Internal: 15, Leaf: 16, Depth: 8
Size 500: built in 10 ms
  Internal: 75, Leaf: 76, Depth: 12
Size 1000: built in 20 ms
  Internal: 150, Leaf: 151, Depth: 15
=== Scale test completed ===

=== Test: DT Lookup Performance ===
Performing 10000 lookups...
10000 lookups in 50 ms (avg 5.00 us per lookup)
=== Performance test completed ===
```

---

## 更简单的方案：扩展现有测试

如果不想创建新文件，可以扩展现有的测试：

```bash
# 检查是否有 DT 测试文件
ls tests/ | grep dt
```

如果已经有测试文件，只需添加测试函数：

```c
// 在现有的 tests/test-dt-*.c 中添加

static void
test_bulk_build(void)
{
    struct decision_tree dt;
    struct rculist rules;
    struct cls_rule *rule_array[100];
    
    printf("\n=== Testing Bulk Build ===\n");
    
    dt_init(&dt);
    rculist_init(&rules);
    
    // 创建规则
    for (int i = 0; i < 100; i++) {
        struct match match;
        match_init_catchall(&match);
        match_set_nw_src(&match, htonl(0x0a000000 + i));
        
        rule_array[i] = xmalloc(sizeof(struct cls_rule));
        cls_rule_init(rule_array[i], &match, 100 - i);
        
        rculist_push_back(&rules, &rule_array[i]->node);
    }
    
    // 全量建树
    printf("Building tree with 100 rules...\n");
    struct dt_node *root = dt_build_tree(&rules, 100, 10);
    
    if (root) {
        printf("✓ Tree built successfully\n");
        ovsrcu_set(&dt.root, root);
        
        // 测试查找
        int correct = 0;
        for (int i = 0; i < 100; i++) {
            struct flow flow;
            memset(&flow, 0, sizeof flow);
            flow.nw_src = htonl(0x0a000000 + i);
            
            if (dt_lookup_simple(&dt, &flow)) {
                correct++;
            }
        }
        
        printf("✓ Lookups: %d/100 found\n", correct);
    } else {
        printf("✗ Failed to build tree\n");
    }
    
    // 清理
    dt_destroy(&dt);
    for (int i = 0; i < 100; i++) {
        cls_rule_destroy(rule_array[i]);
        free(rule_array[i]);
    }
    
    printf("=== Test completed ===\n");
}
```

---

## 快速验证脚本

创建一个简单的验证脚本：

```bash
#!/bin/bash
# test-dt-quick.sh

echo "=== Quick DT Test ==="
echo ""

# 编译
echo "1. Compiling..."
make tests/ovstest 2>&1 | grep -E "(error|warning|built)"

if [ $? -eq 0 ]; then
    echo "✓ Compilation successful"
else
    echo "✗ Compilation failed"
    exit 1
fi

# 运行测试
echo ""
echo "2. Running tests..."
./tests/ovstest test-dt-bulk

echo ""
echo "=== Test completed ==="
```

---

## 调试技巧

### 1. 添加详细日志

```c
// 在 dt_build_tree() 中添加日志

struct dt_node *
dt_build_tree(struct rculist *rules, size_t n_rules, size_t max_leaf_size)
{
    VLOG_INFO("dt_build_tree: n_rules=%zu, max_leaf_size=%zu", 
              n_rules, max_leaf_size);
    
    if (n_rules <= max_leaf_size) {
        VLOG_INFO("Creating leaf node with %zu rules", n_rules);
        // ...
    }
    
    // ...
    
    VLOG_INFO("Split field: %s", split_field->name);
    VLOG_INFO("Left: %zu rules, Right: %zu rules", n_left, n_right);
    
    // ...
}
```

### 2. 可视化树结构

```c
// 添加树打印函数

static void
dt_print_tree(const struct dt_node *node, int depth)
{
    if (!node) return;
    
    for (int i = 0; i < depth; i++) printf("  ");
    
    if (node->type == DT_NODE_LEAF) {
        printf("LEAF: %zu rules\n", node->leaf.n_rules);
    } else {
        printf("INTERNAL: field=%s\n", 
               node->internal.field ? node->internal.field->name : "none");
        
        dt_print_tree(ovsrcu_get(struct dt_node *, &node->internal.left), 
                      depth + 1);
        dt_print_tree(ovsrcu_get(struct dt_node *, &node->internal.right), 
                      depth + 1);
    }
}

// 在测试中调用
printf("\nTree structure:\n");
dt_print_tree(root, 0);
```

---

## 常见问题

### Q1: 编译错误 - 找不到 dt_build_tree

```bash
# 确保 dt-classifier.c 被编译
grep "dt-classifier.c" lib/automake.mk

# 如果没有，添加:
lib_libopenvswitch_la_SOURCES += \
    lib/dt-classifier.c \
    lib/dt-classifier.h
```

### Q2: 链接错误 - undefined reference

```bash
# 检查链接顺序
make V=1 tests/ovstest 2>&1 | grep "dt-classifier"

# 确保 libopenvswitch.la 包含 dt-classifier.o
```

### Q3: 运行时崩溃

```c
// 添加断言检查
ovs_assert(rules != NULL);
ovs_assert(n_rules > 0);
ovs_assert(max_leaf_size > 0);

// 使用 valgrind 检查内存问题
valgrind --leak-check=full ./tests/ovstest test-dt-bulk
```

---

## 总结

### 为什么全量建树适合测试？

```
✅ 优势:
  1. 实现简单 - 一次性建树，无需处理插入逻辑
  2. 独立测试 - 不依赖 TSS 或 classifier 集成
  3. 快速验证 - 30 分钟内完成测试
  4. 容易调试 - 树结构固定，易于分析
  5. 性能基准 - 可以测试最优情况

⚠️ 限制:
  1. 不是生产方案 - 最终仍需集成渐进式
  2. 功能有限 - 不支持动态增删
  3. 与 OVS 不兼容 - 无法直接用于 classifier
```

### 测试流程

```
1. 创建测试程序 (test-dt-bulk.c)    → 30 分钟
2. 添加到构建系统 (automake.mk)     → 5 分钟
3. 编译测试                         → 2 分钟
4. 运行并验证                       → 5 分钟
5. 调试和优化                       → 根据需要

总计: ~1 小时完成基础测试
```

### 下一步

```
测试成功后:
  1. ✅ 验证 DT 核心功能正常
  2. ✅ 确认查找逻辑正确
  3. ✅ 获得性能基准数据
  
然后:
  → 开始集成到 classifier (渐进式)
  → 或继续优化全量建树性能
```

---

**创建时间**: 2025-10-19  
**用途**: 快速测试 DT 全量建树功能  
**估计时间**: 30-60 分钟  
**难度**: ⭐ 简单
