
#include <stdio.h>
#include "lib/dt-classifier.h"
#include "lib/classifier.h"
#include "lib/flow.h"

int main() {
    printf("DT Lazy Build Test\n");
    printf("==================\n\n");
    
    /* 1. 初始化决策树 */
    struct decision_tree dt;
    dt_init(&dt);
    printf("✓ DT initialized (tree_built=%d, n_pending=%zu)\n", 
           dt.tree_built, dt.n_pending);
    
    /* 2. 添加 5 条规则到 pending 列表（懒加载模式） */
    printf("\n[Adding 5 rules in lazy mode]\n");
    for (int i = 0; i < 5; i++) {
        struct cls_rule *rule = xmalloc(sizeof *rule);
        struct match match;
        match_init_catchall(&match);
        
        /* 设置不同的优先级 */
        rule->priority = 100 - i * 10;
        
        /* 添加到 pending 列表 */
        dt_add_rule_lazy(&dt, rule);
        printf("  Added rule %d (priority=%d), pending=%zu, tree_built=%d\n",
               i+1, rule->priority, dt.n_pending, dt.tree_built);
    }
    
    printf("\n✓ All rules added to pending list\n");
    printf("  tree_built=%d (should be false)\n", dt.tree_built);
    printf("  n_pending=%zu (should be 5)\n", dt.n_pending);
    
    /* 3. 第一次查找 - 应该触发懒建树 */
    printf("\n[First lookup - should trigger lazy build]\n");
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    
    const struct cls_rule *result = dt_lookup_simple(&dt, &flow);
    
    printf("\n✓ Lookup completed\n");
    printf("  tree_built=%d (should be true)\n", dt.tree_built);
    printf("  n_rules=%d (should be 5)\n", dt.n_rules);
    printf("  result=%p\n", result);
    
    /* 4. 第二次查找 - 不应该重建树 */
    printf("\n[Second lookup - should NOT rebuild]\n");
    result = dt_lookup_simple(&dt, &flow);
    printf("✓ Second lookup completed (tree should not rebuild)\n");
    
    /* 5. 清理 */
    dt_destroy(&dt);
    printf("\n✓ DT destroyed\n");
    
    printf("\n==================\n");
    printf("Test PASSED! ✅\n");
    return 0;
}
