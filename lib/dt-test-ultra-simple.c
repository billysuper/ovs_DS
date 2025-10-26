/* Very simple test to debug lookup issue */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dt-classifier.h"
#include "classifier.h"
#include "flow.h"
#include "openvswitch/match.h"
#include "rculist.h"

int main(void)
{
    printf("=== Ultra-Simple DT Test ===\n\n");
    
    /* Step 1: Initialize */
    struct decision_tree dt;
    dt_init(&dt);
    printf("1. DT initialized\n");
    
    /* Step 2: Create rule */
    struct cls_rule *rule = xmalloc(sizeof *rule);
    struct match match;
    match_init_catchall(&match);
    cls_rule_init(rule, &match, 100);
    printf("2. Rule created with priority %d\n", rule->priority);
    
    /* Step 3: Insert */
    bool ok = dt_insert_rule(&dt, rule, 1);
    printf("3. Insert returned: %s\n", ok ? "TRUE" : "FALSE");
    
    /* Step 4: Check root */
    struct dt_node *root = ovsrcu_get_protected(struct dt_node *, &dt.root);
    printf("4. Root exists: %s\n", root ? "YES" : "NO");
    
    if (root) {
        printf("   Root type: %s\n", 
               root->type == DT_NODE_LEAF ? "LEAF" : "INTERNAL");
        
        if (root->type == DT_NODE_LEAF) {
            printf("   Leaf n_rules: %zu\n", root->leaf.n_rules);
            
            /* Check if rules list is empty */
            bool is_empty = rculist_is_empty(&root->leaf.rules);
            printf("   Rules list is_empty: %s\n", is_empty ? "YES (BAD!)" : "NO (good)");
            
            /* Try to iterate */
            printf("   Trying to iterate rules...\n");
            const struct cls_rule *iter;
            int count = 0;
            RCULIST_FOR_EACH (iter, node, &root->leaf.rules) {
                printf("     Rule %d: priority=%d\n", count++, iter->priority);
            }
            printf("   Found %d rules\n", count);
        }
    }
    
    /* Step 5: Try lookup */
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    
    printf("5. Trying lookup_simple...\n");
    const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
    printf("   Result: %s\n", found ? "FOUND!" : "NULL (failed)");
    
    if (found) {
        printf("   Found rule priority: %d\n", found->priority);
    }
    
    /* Cleanup */
    dt_destroy(&dt);
    cls_rule_destroy(rule);
    free(rule);
    
    return 0;
}
