/* Simple test for decision tree classifier */

#include <config.h>
#include "dt-classifier.h"
#include "classifier.h"
#include "flow.h"
#include "openvswitch/match.h"
#include "openvswitch/vlog.h"
#include "util.h"

VLOG_DEFINE_THIS_MODULE(dt_classifier_test);

static void
test_basic_insertion(void)
{
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Create a simple rule */
    struct cls_rule *rule = xmalloc(sizeof *rule);
    struct match match;
    
    match_init_catchall(&match);
    match_set_in_port(&match, 1);
    
    cls_rule_init(rule, &match, 100);
    
    /* Insert rule */
    bool ok = dt_insert_rule(&dt, rule, 1);
    
    printf("Basic insertion: %s\n", ok ? "PASS" : "FAIL");
    
    /* Get statistics */
    size_t n_rules, n_internal, n_leaf, max_depth;
    dt_get_stats(&dt, &n_rules, &n_internal, &n_leaf, &max_depth);
    
    printf("Statistics: rules=%zu, internal=%zu, leaf=%zu, depth=%zu\n",
           n_rules, n_internal, n_leaf, max_depth);
    
    /* Cleanup */
    dt_destroy(&dt);
    cls_rule_destroy(rule);
    free(rule);
}

static void
test_basic_lookup(void)
{
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Create and insert rule */
    struct cls_rule *rule = xmalloc(sizeof *rule);
    struct match match;
    
    match_init_catchall(&match);
    match_set_in_port(&match, 1);
    match_set_dl_type(&match, htons(ETH_TYPE_IP));
    
    cls_rule_init(rule, &match, 100);
    dt_insert_rule(&dt, rule, 1);
    
    /* Create flow to lookup */
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.in_port.ofp_port = 1;
    flow.dl_type = htons(ETH_TYPE_IP);
    
    /* Perform lookup */
    struct flow_wildcards wc;
    const struct cls_rule *found = dt_lookup(&dt, 1, &flow, &wc);
    
    printf("Basic lookup: %s\n", found ? "PASS" : "FAIL");
    
    if (found) {
        printf("  Found rule with priority %d\n", found->priority);
    }
    
    /* Cleanup */
    dt_destroy(&dt);
    cls_rule_destroy(rule);
    free(rule);
}

static void
test_multiple_rules(void)
{
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Insert multiple rules with different priorities */
    for (int i = 0; i < 5; i++) {
        struct cls_rule *rule = xmalloc(sizeof *rule);
        struct match match;
        
        match_init_catchall(&match);
        match_set_in_port(&match, i + 1);
        
        cls_rule_init(rule, &match, (i + 1) * 10);
        dt_insert_rule(&dt, rule, 1);
    }
    
    size_t n_rules, n_internal, n_leaf, max_depth;
    dt_get_stats(&dt, &n_rules, &n_internal, &n_leaf, &max_depth);
    
    printf("Multiple rules: rules=%zu, internal=%zu, leaf=%zu, depth=%zu\n",
           n_rules, n_internal, n_leaf, max_depth);
    
    printf("Multiple rules test: %s\n", n_rules == 5 ? "PASS" : "FAIL");
    
    dt_destroy(&dt);
}

int
main(void)
{
    printf("=== Decision Tree Classifier Tests ===\n\n");
    
    test_basic_insertion();
    printf("\n");
    
    test_basic_lookup();
    printf("\n");
    
    test_multiple_rules();
    printf("\n");
    
    printf("=== All tests completed ===\n");
    
    return 0;
}
