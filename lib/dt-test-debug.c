/* Debug test for dt lookup */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dt-classifier.h"
#include "classifier.h"
#include "flow.h"
#include "openvswitch/match.h"

int main(void)
{
    printf("=== DT Lookup Debug Test ===\n");
    
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Create and insert rule */
    struct cls_rule *rule = xmalloc(sizeof *rule);
    struct match match;
    
    match_init_catchall(&match);
    match_set_in_port(&match, 1);
    
    cls_rule_init(rule, &match, 100);
    
    printf("1. Inserting rule with priority %d\n", rule->priority);
    bool ok = dt_insert_rule(&dt, rule, 1);
    printf("   Insert result: %s\n", ok ? "OK" : "FAILED");
    
    /* Get statistics */
    size_t n_rules, n_internal, n_leaf, max_depth;
    dt_get_stats(&dt, &n_rules, &n_internal, &n_leaf, &max_depth);
    printf("2. Stats after insert: rules=%zu, leaves=%zu\n", n_rules, n_leaf);
    
    /* Try simple lookup */
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.in_port.ofp_port = 1;
    
    printf("3. Performing lookup...\n");
    const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
    
    if (found) {
        printf("   SUCCESS! Found rule with priority %d\n", found->priority);
    } else {
        printf("   FAILED! dt_lookup_simple returned NULL\n");
        
        /* Try with version parameter */
        printf("4. Trying dt_lookup with version...\n");
        struct flow_wildcards wc;
        found = dt_lookup(&dt, 1, &flow, &wc);
        
        if (found) {
            printf("   SUCCESS with dt_lookup! Found rule with priority %d\n", found->priority);
        } else {
            printf("   STILL FAILED! dt_lookup also returned NULL\n");
        }
    }
    
    /* Cleanup */
    dt_destroy(&dt);
    cls_rule_destroy(rule);
    free(rule);
    
    return 0;
}
