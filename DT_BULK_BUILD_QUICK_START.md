# DT 全量建树测试 - 快速开始

## 🚀 快速运行（3 步）

### Windows (PowerShell)

```powershell
# 1. 确保在 OVS 根目录
cd d:\ovs_DS

# 2. 运行测试脚本
.\run-dt-bulk-test.ps1

# 或手动运行:
make tests/ovstest
.\tests\ovstest test-dt-bulk
```

### Linux/WSL (Bash)

```bash
# 1. 确保在 OVS 根目录
cd /path/to/ovs

# 2. 运行测试脚本
chmod +x run-dt-bulk-test.sh
./run-dt-bulk-test.sh

# 或手动运行:
make tests/ovstest
./tests/ovstest test-dt-bulk
```

---

## 📁 文件清单

```
已创建的文件:
✓ tests/test-dt-bulk.c              # 测试代码
✓ run-dt-bulk-test.sh               # Linux 运行脚本
✓ run-dt-bulk-test.ps1              # Windows 运行脚本
✓ DT_BULK_BUILD_TEST_PLAN.md        # 详细测试计划
✓ DT_BULK_BUILD_QUICK_START.md      # 本文件

需要的现有文件:
✓ lib/dt-classifier.c               # DT 实现
✓ lib/dt-classifier.h               # DT 头文件
```

---

## ⚙️ 构建系统配置

### 方法 1: 添加到 tests/automake.mk

```makefile
# 在 tests/automake.mk 中添加:

tests_ovstest_SOURCES += \
    tests/test-dt-bulk.c
```

### 方法 2: 创建独立测试程序

```makefile
# 在 tests/automake.mk 中添加:

noinst_PROGRAMS += tests/test-dt-bulk

tests_test_dt_bulk_SOURCES = tests/test-dt-bulk.c
tests_test_dt_bulk_LDADD = lib/libopenvswitch.la
```

---

## 🧪 测试内容

### Test 1: 基础功能 (100 条规则)
```
- 创建 100 条测试规则
- 全量建树 (dt_build_tree)
- 验证 100 次查找
- 输出树统计信息
```

### Test 2: 规模测试 (10, 50, 100, 500, 1000)
```
- 测试不同规模的建树时间
- 对比树结构 (节点数、深度)
- 验证性能扩展性
```

### Test 3: 查找性能 (10000 次)
```
- 1000 条规则
- 10000 次查找
- 计算平均查找时间
```

---

## 📊 预期输出

```
╔══════════════════════════════════════════════╗
║   Decision Tree Bulk Build Test Suite       ║
╚══════════════════════════════════════════════╝

=== Test: DT Bulk Build Basic ===
Creating 100 test rules...
Building tree with dt_build_tree()...
Tree built in 2 ms
✓ Tree built successfully
Testing lookups...
Lookup test: 100/100 correct ✓ PASS
Tree statistics:
  Rules: 100
  Internal nodes: 15
  Leaf nodes: 16
  Max depth: 8
=== Test completed ===

=== Test: DT Bulk Build Scale ===
Size   10: built in   0 ms - Internal:   1, Leaf:   2, Depth:  2
Size   50: built in   1 ms - Internal:   7, Leaf:   8, Depth:  5
Size  100: built in   2 ms - Internal:  15, Leaf:  16, Depth:  8
Size  500: built in  10 ms - Internal:  75, Leaf:  76, Depth: 12
Size 1000: built in  20 ms - Internal: 150, Leaf: 151, Depth: 15
=== Scale test completed ===

=== Test: DT Lookup Performance ===
Creating 1000 rules...
Building tree...
Performing 10000 lookups...
10000 lookups in 50 ms (avg 5.00 us per lookup)
=== Performance test completed ===

╔══════════════════════════════════════════════╗
║   All tests completed                        ║
╚══════════════════════════════════════════════╝
```

---

## 🐛 故障排除

### 问题 1: 找不到 dt_build_tree

```bash
# 检查符号
nm lib/.libs/libopenvswitch.a | grep dt_build_tree

# 如果找不到，检查 lib/automake.mk:
grep "dt-classifier.c" lib/automake.mk

# 应该有类似这样的行:
# lib_libopenvswitch_la_SOURCES += lib/dt-classifier.c
```

### 问题 2: 编译错误

```bash
# 清理重新编译
make clean
make tests/ovstest

# 查看详细编译输出
make V=1 tests/ovstest
```

### 问题 3: 链接错误

```bash
# 检查依赖
ldd tests/ovstest  # Linux
otool -L tests/ovstest  # macOS

# 确保 libopenvswitch 包含 dt-classifier.o
ar -t lib/.libs/libopenvswitch.a | grep dt-classifier
```

### 问题 4: 运行时崩溃

```bash
# 使用 valgrind (Linux)
valgrind --leak-check=full ./tests/ovstest test-dt-bulk

# 使用 gdb 调试
gdb ./tests/ovstest
(gdb) run test-dt-bulk
(gdb) bt  # 查看堆栈
```

---

## 📈 性能基准

### 预期性能 (参考)

| 规则数 | 建树时间 | 查找时间 | 树深度 |
|--------|----------|----------|--------|
| 10     | <1 ms    | <1 us    | 2-3    |
| 100    | 1-2 ms   | 1-2 us   | 6-8    |
| 1000   | 10-20 ms | 2-5 us   | 10-15  |
| 10000  | 100-200 ms | 5-10 us | 15-20  |

**注意**: 实际性能取决于 CPU、内存、规则分布等因素。

---

## 🔧 自定义测试

### 修改规则数量

```c
// 在 test-dt-bulk.c 中修改:

// 基础测试规则数
#define BASIC_TEST_SIZE 100  // 改为 500, 1000 等

// 规模测试大小
int sizes[] = {10, 50, 100, 500, 1000, 5000};  // 添加更多
```

### 修改叶子节点大小

```c
// 影响树结构的关键参数

struct dt_node *root = dt_build_tree(&rules_list, n_rules, 
                                     10);  // ← 修改这里
//                                   ^^
//                                   max_leaf_size
// 更小 → 更深的树，更多内部节点
// 更大 → 更浅的树，更大的叶子
```

### 添加自定义测试

```c
// 在 test-dt-bulk.c 中添加:

static void
test_dt_my_custom_test(void)
{
    printf("\n=== My Custom Test ===\n");
    
    // 你的测试代码
    
    printf("=== Custom test completed ===\n");
}

// 在 test_dt_bulk() 中调用:
static void
test_dt_bulk(int argc OVS_UNUSED, char *argv[] OVS_UNUSED)
{
    test_dt_bulk_basic();
    test_dt_bulk_scale();
    test_dt_bulk_lookup_perf();
    test_dt_my_custom_test();  // ← 添加这里
}
```

---

## 📝 下一步

### 测试成功后

```
✓ DT 核心功能验证完成
✓ 获得性能基准数据
✓ 确认树结构正确

接下来:
1. 分析性能瓶颈
2. 优化建树算法
3. 开始集成到 classifier
4. 实现渐进式插入
```

### 集成到 Classifier

```
选项 A: 继续全量建树方式
  - 在 classifier_init() 后批量建树
  - 适合已知所有规则的场景
  - 需要修改 OVS 架构

选项 B: 实现渐进式插入 (推荐)
  - dt_insert_rule() 逐条插入
  - 与 TSS 行为一致
  - 符合 OVS 设计
  - 按需重建优化
```

---

## 📚 相关文档

- **DT_BULK_BUILD_TEST_PLAN.md** - 完整测试计划
- **DT_INITIALIZATION_STRATEGY.md** - 初始化策略分析
- **DT_INCREMENTAL_VS_BULK_ANALYSIS.md** - 渐进式 vs 全量对比
- **DT_INTEGRATION_DESIGN.md** - 集成设计方案
- **DT_NEXT_STEPS.md** - 下一步计划

---

## ✅ 检查清单

运行测试前:
- [ ] 确认 `lib/dt-classifier.c` 存在
- [ ] 确认 `lib/dt-classifier.h` 存在
- [ ] 确认 `tests/test-dt-bulk.c` 已创建
- [ ] 确认测试文件在构建系统中

运行测试:
- [ ] 编译成功 (`make tests/ovstest`)
- [ ] 基础测试通过 (100/100 正确)
- [ ] 规模测试通过 (所有规模)
- [ ] 性能测试完成

分析结果:
- [ ] 查找正确率 100%
- [ ] 建树时间符合预期
- [ ] 树深度合理
- [ ] 查找性能可接受

---

**创建时间**: 2025-10-19  
**预计时间**: 30-60 分钟  
**难度**: ⭐ 简单  
**状态**: 就绪 ✓
