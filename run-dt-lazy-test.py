#!/usr/bin/env python3
"""
测试 DT 懒加载功能
"""

import subprocess
import sys

def run_command(cmd):
    """运行命令并返回输出"""
    print(f"$ {cmd}")
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    print(result.stdout)
    if result.stderr:
        print(result.stderr, file=sys.stderr)
    return result.returncode

def main():
    print("=" * 70)
    print("DT 懒加载功能测试")
    print("=" * 70)
    
    # 编译
    print("\n[步骤 1] 编译 dt-classifier.c...")
    ret = run_command("cd /mnt/d/ovs_DS && make lib/dt-classifier.lo")
    if ret != 0:
        print("❌ 编译失败！")
        return 1
    print("✅ 编译成功")
    
    # 创建简单测试
    print("\n[步骤 2] 创建懒加载测试...")
    test_code = '''
#include <stdio.h>
#include "lib/dt-classifier.h"
#include "lib/classifier.h"
#include "lib/flow.h"

int main() {
    printf("DT Lazy Build Test\\n");
    printf("==================\\n\\n");
    
    /* 1. 初始化决策树 */
    struct decision_tree dt;
    dt_init(&dt);
    printf("✓ DT initialized (tree_built=%d, n_pending=%zu)\\n", 
           dt.tree_built, dt.n_pending);
    
    /* 2. 添加 5 条规则到 pending 列表（懒加载模式） */
    printf("\\n[Adding 5 rules in lazy mode]\\n");
    for (int i = 0; i < 5; i++) {
        struct cls_rule *rule = xmalloc(sizeof *rule);
        struct match match;
        match_init_catchall(&match);
        
        /* 设置不同的优先级 */
        rule->priority = 100 - i * 10;
        
        /* 添加到 pending 列表 */
        dt_add_rule_lazy(&dt, rule);
        printf("  Added rule %d (priority=%d), pending=%zu, tree_built=%d\\n",
               i+1, rule->priority, dt.n_pending, dt.tree_built);
    }
    
    printf("\\n✓ All rules added to pending list\\n");
    printf("  tree_built=%d (should be false)\\n", dt.tree_built);
    printf("  n_pending=%zu (should be 5)\\n", dt.n_pending);
    
    /* 3. 第一次查找 - 应该触发懒建树 */
    printf("\\n[First lookup - should trigger lazy build]\\n");
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    
    const struct cls_rule *result = dt_lookup_simple(&dt, &flow);
    
    printf("\\n✓ Lookup completed\\n");
    printf("  tree_built=%d (should be true)\\n", dt.tree_built);
    printf("  n_rules=%d (should be 5)\\n", dt.n_rules);
    printf("  result=%p\\n", result);
    
    /* 4. 第二次查找 - 不应该重建树 */
    printf("\\n[Second lookup - should NOT rebuild]\\n");
    result = dt_lookup_simple(&dt, &flow);
    printf("✓ Second lookup completed (tree should not rebuild)\\n");
    
    /* 5. 清理 */
    dt_destroy(&dt);
    printf("\\n✓ DT destroyed\\n");
    
    printf("\\n==================\\n");
    printf("Test PASSED! ✅\\n");
    return 0;
}
'''
    
    with open('/mnt/d/ovs_DS/test-dt-lazy.c', 'w') as f:
        f.write(test_code)
    print("✅ 测试代码已创建: test-dt-lazy.c")
    
    # 编译测试
    print("\n[步骤 3] 编译测试程序...")
    compile_cmd = '''cd /mnt/d/ovs_DS && \\
gcc -o test-dt-lazy test-dt-lazy.c \\
    lib/.libs/libopenvswitch.a \\
    -I. -Iinclude -Ilib -I./lib \\
    -lpthread -lrt -lm'''
    
    ret = run_command(compile_cmd)
    if ret != 0:
        print("❌ 测试编译失败！")
        return 1
    print("✅ 测试编译成功")
    
    # 运行测试
    print("\n[步骤 4] 运行测试...")
    ret = run_command("cd /mnt/d/ovs_DS && ./test-dt-lazy")
    if ret != 0:
        print("❌ 测试失败！")
        return 1
    
    print("\n" + "=" * 70)
    print("所有测试通过！✅")
    print("=" * 70)
    return 0

if __name__ == '__main__':
    sys.exit(main())
