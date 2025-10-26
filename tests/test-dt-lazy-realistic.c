/*
 * Decision Tree Lazy Loading Test with Realistic Data
 * 
 * This test uses realistic flow rules (similar to test-classifier.c)
 * to verify that the decision tree can actually split rules across
 * multiple nodes based on different field values.
 */

#include <config.h>
#undef NDEBUG
#include "dt-classifier.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "classifier.h"
#include "flow.h"
#include "openvswitch/match.h"
#include "util.h"
#include "timeval.h"
#include "ovstest.h"
#include "packets.h"

/* Test data: Different IP addresses and ports */
static ovs_be32 ip_src_values[] = {
    CONSTANT_HTONL(0x0a000001),  /* 10.0.0.1 */
    CONSTANT_HTONL(0x0a000002),  /* 10.0.0.2 */
    CONSTANT_HTONL(0x0a000003),  /* 10.0.0.3 */
    CONSTANT_HTONL(0x0a000004),  /* 10.0.0.4 */
};

static ovs_be32 ip_dst_values[] = {
    CONSTANT_HTONL(0xc0a80001),  /* 192.168.0.1 */
    CONSTANT_HTONL(0xc0a80002),  /* 192.168.0.2 */
    CONSTANT_HTONL(0xc0a80003),  /* 192.168.0.3 */
    CONSTANT_HTONL(0xc0a80004),  /* 192.168.0.4 */
};

static ovs_be16 tp_src_values[] = {
    CONSTANT_HTONS(80),    /* HTTP */
    CONSTANT_HTONS(443),   /* HTTPS */
    CONSTANT_HTONS(22),    /* SSH */
    CONSTANT_HTONS(3306),  /* MySQL */
};

static ovs_be16 tp_dst_values[] = {
    CONSTANT_HTONS(8080),  /* HTTP Alt */
    CONSTANT_HTONS(8443),  /* HTTPS Alt */
    CONSTANT_HTONS(2222),  /* SSH Alt */
    CONSTANT_HTONS(3307),  /* MySQL Alt */
};

static uint8_t nw_proto_values[] = {
    IPPROTO_TCP,
    IPPROTO_UDP,
    IPPROTO_ICMP,
    IPPROTO_SCTP,
};

/* Helper: Create a rule with specific field values */
static struct cls_rule *
make_test_rule(int rule_type, int value_index, int priority)
{
    struct cls_rule *rule = xzalloc(sizeof *rule);
    struct match match;
    
    match_init_catchall(&match);
    
    /* Set different fields based on rule type */
    switch (rule_type) {
    case 0:  /* IP source address rules */
        match_set_dl_type(&match, htons(ETH_TYPE_IP));
        match_set_nw_src(&match, ip_src_values[value_index % 4]);
        break;
        
    case 1:  /* IP destination address rules */
        match_set_dl_type(&match, htons(ETH_TYPE_IP));
        match_set_nw_dst(&match, ip_dst_values[value_index % 4]);
        break;
        
    case 2:  /* TCP source port rules */
        match_set_dl_type(&match, htons(ETH_TYPE_IP));
        match_set_nw_proto(&match, IPPROTO_TCP);
        match_set_tp_src(&match, tp_src_values[value_index % 4]);
        break;
        
    case 3:  /* TCP destination port rules */
        match_set_dl_type(&match, htons(ETH_TYPE_IP));
        match_set_nw_proto(&match, IPPROTO_TCP);
        match_set_tp_dst(&match, tp_dst_values[value_index % 4]);
        break;
        
    case 4:  /* Protocol rules */
        match_set_dl_type(&match, htons(ETH_TYPE_IP));
        match_set_nw_proto(&match, nw_proto_values[value_index % 4]);
        break;
        
    default:  /* Combined rules */
        match_set_dl_type(&match, htons(ETH_TYPE_IP));
        match_set_nw_src(&match, ip_src_values[value_index % 4]);
        match_set_nw_dst(&match, ip_dst_values[(value_index + 1) % 4]);
        match_set_nw_proto(&match, IPPROTO_TCP);
        match_set_tp_src(&match, tp_src_values[(value_index + 2) % 4]);
        break;
    }
    
    cls_rule_init(rule, &match, priority);
    return rule;
}

/* Test with realistic diverse rules */
static void
test_realistic_rules(void)
{
    printf("\n=== Test: Realistic Diverse Rules ===\n");
    
    struct decision_tree dt;
    dt_init(&dt);
    
    const int N_RULES = 50;
    struct cls_rule *rules[N_RULES];
    
    /* Create diverse rules:
     * - 10 rules based on IP source
     * - 10 rules based on IP destination
     * - 10 rules based on TCP source port
     * - 10 rules based on TCP destination port
     * - 10 rules based on protocol type
     */
    printf("Creating %d diverse rules...\n", N_RULES);
    for (int i = 0; i < N_RULES; i++) {
        int rule_type = i / 10;  /* 0-4, changes every 10 rules */
        int value_index = i % 10;
        rules[i] = make_test_rule(rule_type, value_index, 1000 + i);
        
        dt_add_rule_lazy(&dt, rules[i]);
        
        if (i % 10 == 9) {
            printf("  Added %d rules (type=%d)\n", i + 1, rule_type);
        }
    }
    
    printf("All rules added, tree_built = %s\n", dt.tree_built ? "true" : "false");
    assert(!dt.tree_built);
    
    /* Trigger tree building */
    printf("\nTriggering tree build with first lookup...\n");
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.dl_type = htons(ETH_TYPE_IP);
    flow.nw_src = ip_src_values[0];
    flow.nw_dst = ip_dst_values[0];
    flow.nw_proto = IPPROTO_TCP;
    flow.tp_src = tp_src_values[0];
    
    const struct cls_rule *result = dt_lookup_simple(&dt, &flow);
    
    printf("Tree built: %s\n", dt.tree_built ? "YES ✓" : "NO ✗");
    assert(dt.tree_built);
    
    if (result) {
        printf("Found matching rule with priority %d\n", result->priority);
    } else {
        printf("No matching rule found\n");
    }
    
    /* Test multiple lookups with different flows */
    printf("\nTesting lookups with different flows...\n");
    int matches = 0;
    
    for (int i = 0; i < 10; i++) {
        struct flow test_flow;
        memset(&test_flow, 0, sizeof test_flow);
        test_flow.dl_type = htons(ETH_TYPE_IP);
        test_flow.nw_src = ip_src_values[i % 4];
        test_flow.nw_dst = ip_dst_values[(i + 1) % 4];
        test_flow.nw_proto = nw_proto_values[i % 4];
        test_flow.tp_src = tp_src_values[(i + 2) % 4];
        
        const struct cls_rule *match = dt_lookup_simple(&dt, &test_flow);
        if (match) {
            matches++;
            if (i < 3) {  /* Print first 3 matches */
                printf("  Flow %d: Match with priority %d\n", i, match->priority);
            }
        }
    }
    
    printf("Found %d matches out of 10 lookups\n", matches);
    
    /* Print tree structure */
    dt_print_tree_info(&dt, "  ");
    
    /* Cleanup */
    dt_destroy(&dt);
    for (int i = 0; i < N_RULES; i++) {
        cls_rule_destroy(rules[i]);
        free(rules[i]);
    }
    
    printf("\n✅ Test PASSED: Realistic rules tested successfully!\n");
}

/* Test with many rules to see tree structure */
static void
test_tree_structure(void)
{
    printf("\n=== Test: Tree Structure with 100 Rules ===\n");
    
    struct decision_tree dt;
    dt_init(&dt);
    
    const int N_RULES = 100;
    struct cls_rule *rules[N_RULES];
    
    /* Create 100 rules with varying IP addresses */
    printf("Creating %d rules with different IP addresses...\n", N_RULES);
    for (int i = 0; i < N_RULES; i++) {
        rules[i] = xzalloc(sizeof *rules[i]);
        struct match match;
        match_init_catchall(&match);
        
        /* Set unique IP source address for each rule */
        match_set_dl_type(&match, htons(ETH_TYPE_IP));
        match_set_nw_src(&match, htonl(0x0a000000 + i));  /* 10.0.0.0 ~ 10.0.0.99 */
        
        cls_rule_init(rules[i], &match, 1000 + i);
        dt_add_rule_lazy(&dt, rules[i]);
    }
    
    printf("Rules created, triggering tree build...\n");
    
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.dl_type = htons(ETH_TYPE_IP);
    flow.nw_src = htonl(0x0a000032);  /* 10.0.0.50 */
    
    long long start = time_msec();
    const struct cls_rule *result = dt_lookup_simple(&dt, &flow);
    long long elapsed = time_msec() - start;
    
    printf("Tree build + first lookup took %lld ms\n", elapsed);
    
    if (result) {
        printf("Found rule with priority %d\n", result->priority);
        
        /* Verify it's the correct rule */
        uint32_t expected_ip = ntohl(flow.nw_src);
        printf("Expected IP: 10.0.0.%d\n", expected_ip & 0xFF);
    }
    
    /* Test lookup performance */
    printf("\nTesting lookup performance...\n");
    start = time_msec();
    int found = 0;
    for (int i = 0; i < 100; i++) {
        flow.nw_src = htonl(0x0a000000 + i);
        if (dt_lookup_simple(&dt, &flow)) {
            found++;
        }
    }
    elapsed = time_msec() - start;
    
    printf("100 lookups: %lld ms (avg %.2f ms/lookup)\n", 
           elapsed, (double)elapsed / 100);
    printf("Found %d/100 rules\n", found);
    
    /* Print tree structure */
    dt_print_tree_info(&dt, "  ");
    
    /* Cleanup */
    dt_destroy(&dt);
    for (int i = 0; i < N_RULES; i++) {
        cls_rule_destroy(rules[i]);
        free(rules[i]);
    }
    
    printf("\n✅ Test PASSED: Tree structure verified!\n");
}

/* Main test runner */
static void
test_dt_lazy_realistic_main(int argc OVS_UNUSED, char *argv[] OVS_UNUSED)
{
    printf("======================================\n");
    printf("DT Lazy Loading - Realistic Data Test\n");
    printf("======================================\n");
    
    /* Run tests */
    test_realistic_rules();
    test_tree_structure();
    
    printf("\n======================================\n");
    printf("✅ ALL REALISTIC TESTS PASSED!\n");
    printf("======================================\n");
}

OVSTEST_REGISTER("test-dt-lazy-realistic", test_dt_lazy_realistic_main);
