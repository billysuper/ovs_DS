#include <config.h>
#include "dt-classifier.h"
#include "classifier.h"
#include "flow.h"
#include "openvswitch/match.h"
#include "ovstest.h"

static void
test_dt_basic(void)
{
    struct decision_tree dt;
    struct cls_rule rule1, rule2;
    struct match match;
    struct flow flow;
    const struct cls_rule *result;
    
    /* Initialize */
    dt_init(&dt);
    
    /* Create two simple rules */
    match_init_catchall(&match);
    match_set_in_port(&match, 1);
    cls_rule_init(&rule1, &match, 100);
    
    match_init_catchall(&match);
    match_set_in_port(&match, 2);
    cls_rule_init(&rule2, &match, 200);
    
    /* Get root */
    struct dt_node *root = NULL;
    
    /* Insert rules */
    ovs_assert(dt_insert_rule_simple(&root, &rule1));
    ovs_assert(dt_insert_rule_simple(&rule2));
    
    /* Lookup test */
    memset(&flow, 0, sizeof flow);
    flow.in_port.ofp_port = 1;
    result = dt_lookup_simple(root, &flow);
    ovs_assert(result == &rule1);
    
    /* Cleanup */
    dt_remove_rule_simple(&root, &rule1);
    dt_remove_rule_simple(&root, &rule2);
    dt_node_destroy(root);
    dt_destroy(&dt);
    cls_rule_destroy(&rule1);
    cls_rule_destroy(&rule2);
    
    printf("test_dt_basic: PASSED\n");
}

static void
test_dt_empty(void)
{
    struct decision_tree dt;
    struct flow flow;
    
    dt_init(&dt);
    
    memset(&flow, 0, sizeof flow);
    ovs_assert(dt_lookup_simple(NULL, &flow) == NULL);
    
    dt_destroy(&dt);
    
    printf("test_dt_empty: PASSED\n");
}

static void
run_tests(int argc OVS_UNUSED, char *argv[] OVS_UNUSED)
{
    test_dt_empty();
    test_dt_basic();
}

OVSTEST_REGISTER("test-dt-classifier", run_tests);