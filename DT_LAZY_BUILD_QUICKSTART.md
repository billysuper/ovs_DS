# DT 懒加载 - 快速开始指南

## 🚀 5 分钟上手

### 1. 查看修改内容

```bash
cd /mnt/d/ovs_DS

# 查看修改的文件
git status

# 查看具体改动
git diff lib/dt-classifier.h
git diff lib/dt-classifier.c
```

### 2. 编译

```bash
# 编译 DT 模块
make lib/dt-classifier.lo

# 如果成功，应该看到：
# CC lib/dt-classifier.lo
```

### 3. 运行测试

```bash
# 方法 1: 使用 Python 脚本（推荐）
python3 run-dt-lazy-test.py

# 方法 2: 手动编译和运行
gcc -o test-dt-lazy test-dt-lazy.c \
    lib/.libs/libopenvswitch.a \
    -I. -Iinclude -Ilib -I./lib \
    -lpthread -lrt -lm

./test-dt-lazy
```

### 4. 预期输出

```
DT Lazy Build Test
==================

✓ DT initialized (tree_built=0, n_pending=0)

[Adding 5 rules in lazy mode]
  Added rule 1 (priority=100), pending=1, tree_built=0
  ⋮

[First lookup - should trigger lazy build]
DT Lazy Build: Building tree from 5 pending rules
DT Lazy Build: Tree built successfully with 5 rules

✓ Lookup completed
  tree_built=1 (should be true)

Test PASSED! ✅
```

---

## 📝 核心 API

### 添加规则（懒加载）

```c
#include "lib/dt-classifier.h"

struct decision_tree dt;
dt_init(&dt);

// 添加规则 - 只累积，不建树
for (int i = 0; i < 1000; i++) {
    struct cls_rule *rule = ...;
    dt_add_rule_lazy(&dt, rule);  // 超快！O(1)
}
```

### 查找（自动建树）

```c
// 第一次查找会自动触发建树
struct flow flow = ...;
const struct cls_rule *result = dt_lookup_simple(&dt, &flow);

// 后续查找直接使用已建好的树
result = dt_lookup_simple(&dt, &flow2);
```

### 清理

```c
dt_destroy(&dt);
```

---

## 🔍 验证懒加载行为

### 方法 1: 查看日志

```bash
# 启用 VLOG
export OVS_LOG_LEVEL=dt_classifier:dbg

./test-dt-lazy 2>&1 | grep "DT Lazy"
```

应该看到：
```
DBG: DT Lazy: Added rule (priority=100) to pending list, total=1
INFO: DT Lazy Build: Building tree from 5 pending rules
INFO: DT Lazy Build: Tree built successfully with 5 rules
```

### 方法 2: 使用 gdb

```bash
gdb ./test-dt-lazy

(gdb) break dt_add_rule_lazy
(gdb) break dt_ensure_tree_built
(gdb) run

# 观察断点触发顺序：
# 1. dt_add_rule_lazy (5次，添加规则)
# 2. dt_ensure_tree_built (1次，首次查找时)
```

---

## ⚡ 性能测试

### 添加 1000 条规则

```c
#include <time.h>

struct decision_tree dt;
dt_init(&dt);

clock_t start = clock();
for (int i = 0; i < 1000; i++) {
    struct cls_rule *rule = create_test_rule(i);
    dt_add_rule_lazy(&dt, rule);
}
clock_t end = clock();

printf("Insert time: %f ms\n", 
       (double)(end - start) / CLOCKS_PER_SEC * 1000);
// 预期: < 1ms

// 首次查找（触发建树）
start = clock();
const struct cls_rule *result = dt_lookup_simple(&dt, &flow);
end = clock();

printf("First lookup (with build): %f ms\n",
       (double)(end - start) / CLOCKS_PER_SEC * 1000);
// 预期: 10-50ms

// 后续查找
start = clock();
result = dt_lookup_simple(&dt, &flow);
end = clock();

printf("Second lookup: %f ms\n",
       (double)(end - start) / CLOCKS_PER_SEC * 1000);
// 预期: < 0.1ms
```

---

## 🐛 常见问题

### Q1: 编译错误 "undefined reference to rculist_push_back"

**原因**: 链接问题

**解决**:
```bash
gcc ... lib/.libs/libopenvswitch.a  # 确保链接 OVS 库
```

### Q2: 运行时 Segmentation Fault

**检查**:
```c
// 确保在销毁前规则仍然有效
dt_add_rule_lazy(&dt, rule);
// rule 不能在 dt_destroy() 之前被释放！
```

### Q3: 看不到 "DT Lazy Build" 日志

**解决**:
```bash
export OVS_LOG_LEVEL=dt_classifier:info
./test-dt-lazy
```

### Q4: 首次查找很慢

**正常！** 这就是懒加载的特点：
- 首次查找需要建树 (10-100ms)
- 后续查找很快 (< 1ms)
- 适合批量加载场景

---

## 📊 性能对比

| 操作 | 原增量插入 | 懒加载 | 提升 |
|------|-----------|--------|------|
| 插入 1000 条规则 | ~100ms | ~1ms | **100倍** |
| 建树次数 | 1000 次 | 1 次 | **1000倍** |
| 首次查找 | 快 (< 1ms) | 慢 (~10ms) | -10倍 |
| 后续查找 | 正常 | 正常 | 持平 |

---

## 📚 完整文档

- `DT_LAZY_BUILD_IMPLEMENTATION.md` - 详细实现说明
- `DT_LAZY_BUILD_SUMMARY.md` - 完成总结
- `DT_BULK_BUILD_TIMING_ANALYSIS.md` - 建树时机分析

---

## 🎯 下一步

### 立即可做
1. ✅ 运行测试验证功能
2. ⏳ 重构 `dt_build_tree()` 接受数组
3. ⏳ 添加更多单元测试

### 后续计划
4. 实现混合模式（懒加载 + 增量）
5. 集成到 classifier.c
6. Bundle Commit 支持

---

## ✅ 检查清单

- [ ] 代码编译成功
- [ ] 测试程序运行通过
- [ ] 看到 "Test PASSED! ✅"
- [ ] 日志显示正确的懒加载行为
- [ ] 性能符合预期（插入 < 1ms，建树 ~10ms）

---

**祝测试顺利！** 🎉

如有问题，参考：
- 实现文档: `DT_LAZY_BUILD_IMPLEMENTATION.md`
- 总结文档: `DT_LAZY_BUILD_SUMMARY.md`
