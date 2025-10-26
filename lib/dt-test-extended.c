/* Extended tests for DT classifier - covers more field types */

#include <config.h>
#include <stdio.h>
#include <string.h>
#include "dt-classifier.h"
#include "classifier.h"
#include "flow.h"
#include "openvswitch/match.h"
#include "packets.h"

/* No VLOG needed for simple test */
/* VLOG_DEFINE_THIS_MODULE(dt_extended_test); */

/* Test counter */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) \
    printf("\n=== Test: %s ===\n", name)

#define TEST_RESULT(condition, msg) \
    do { \
        if (condition) { \
            printf("  ✓ PASS: %s\n", msg); \
            tests_passed++; \
        } else { \
            printf("  ✗ FAIL: %s\n", msg); \
            tests_failed++; \
        } \
    } while (0)

/* Test 1: MAC address filtering */
static void
test_mac_address(void)
{
    TEST_START("MAC Address Filtering");
    
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Create rule matching specific source MAC */
    struct cls_rule *rule = xmalloc(sizeof *rule);
    struct match match;
    match_init_catchall(&match);
    
    struct eth_addr src_mac = ETH_ADDR_C(00,11,22,33,44,55);
    match_set_dl_src(&match, src_mac);
    
    cls_rule_init(rule, &match, 100);
    bool inserted = dt_insert_rule(&dt, rule, 1);
    TEST_RESULT(inserted, "Insert MAC rule");
    
    /* Create flow with matching MAC */
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.dl_src = src_mac;
    
    const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
    TEST_RESULT(found != NULL, "Lookup matching MAC");
    TEST_RESULT(found && found->priority == 100, "Correct priority");
    
    /* Test with non-matching MAC */
    struct flow flow2;
    memset(&flow2, 0, sizeof flow2);
    struct eth_addr broadcast = ETH_ADDR_C(ff,ff,ff,ff,ff,ff);
    flow2.dl_src = broadcast;
    
    const struct cls_rule *not_found = dt_lookup_simple(&dt, &flow2);
    TEST_RESULT(not_found == NULL, "No match for different MAC");
    
    dt_destroy(&dt);
    cls_rule_destroy(rule);
    free(rule);
}

/* Test 2: IP address filtering */
static void
test_ip_address(void)
{
    TEST_START("IP Address Filtering");
    
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Create rule matching specific source IP */
    struct cls_rule *rule = xmalloc(sizeof *rule);
    struct match match;
    match_init_catchall(&match);
    
    match_set_dl_type(&match, htons(ETH_TYPE_IP));
    match_set_nw_src(&match, htonl(0x0a000001));  /* 10.0.0.1 */
    
    cls_rule_init(rule, &match, 100);
    bool inserted = dt_insert_rule(&dt, rule, 1);
    TEST_RESULT(inserted, "Insert IP rule");
    
    /* Create flow with matching IP */
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.dl_type = htons(ETH_TYPE_IP);
    flow.nw_src = htonl(0x0a000001);
    
    const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
    TEST_RESULT(found != NULL, "Lookup matching IP");
    
    /* Test with different IP */
    flow.nw_src = htonl(0x0a000002);  /* 10.0.0.2 */
    const struct cls_rule *not_found = dt_lookup_simple(&dt, &flow);
    TEST_RESULT(not_found == NULL, "No match for different IP");
    
    dt_destroy(&dt);
    cls_rule_destroy(rule);
    free(rule);
}

/* Test 3: TCP port filtering */
static void
test_tcp_ports(void)
{
    TEST_START("TCP Port Filtering");
    
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Create rule matching HTTP traffic (port 80) */
    struct cls_rule *rule = xmalloc(sizeof *rule);
    struct match match;
    match_init_catchall(&match);
    
    match_set_dl_type(&match, htons(ETH_TYPE_IP));
    match_set_nw_proto(&match, IPPROTO_TCP);
    match_set_tp_dst(&match, htons(80));
    
    cls_rule_init(rule, &match, 100);
    bool inserted = dt_insert_rule(&dt, rule, 1);
    TEST_RESULT(inserted, "Insert TCP port rule");
    
    /* Create HTTP flow */
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.dl_type = htons(ETH_TYPE_IP);
    flow.nw_proto = IPPROTO_TCP;
    flow.tp_dst = htons(80);
    
    const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
    TEST_RESULT(found != NULL, "Lookup HTTP traffic");
    
    /* Test HTTPS (port 443) - should not match */
    flow.tp_dst = htons(443);
    const struct cls_rule *not_found = dt_lookup_simple(&dt, &flow);
    TEST_RESULT(not_found == NULL, "No match for HTTPS");
    
    dt_destroy(&dt);
    cls_rule_destroy(rule);
    free(rule);
}

/* Test 4: Multiple field combination (5-tuple) */
static void
test_5tuple_match(void)
{
    TEST_START("5-tuple Match");
    
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Create rule with 5-tuple */
    struct cls_rule *rule = xmalloc(sizeof *rule);
    struct match match;
    match_init_catchall(&match);
    
    match_set_dl_type(&match, htons(ETH_TYPE_IP));
    match_set_nw_src(&match, htonl(0x0a000001));      /* 10.0.0.1 */
    match_set_nw_dst(&match, htonl(0xc0a80101));      /* 192.168.1.1 */
    match_set_nw_proto(&match, IPPROTO_TCP);
    match_set_tp_src(&match, htons(12345));
    match_set_tp_dst(&match, htons(80));
    
    cls_rule_init(rule, &match, 100);
    bool inserted = dt_insert_rule(&dt, rule, 1);
    TEST_RESULT(inserted, "Insert 5-tuple rule");
    
    /* Create exactly matching flow */
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.dl_type = htons(ETH_TYPE_IP);
    flow.nw_src = htonl(0x0a000001);
    flow.nw_dst = htonl(0xc0a80101);
    flow.nw_proto = IPPROTO_TCP;
    flow.tp_src = htons(12345);
    flow.tp_dst = htons(80);
    
    const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
    TEST_RESULT(found != NULL, "Lookup exact 5-tuple match");
    
    /* Change one field - should not match */
    flow.tp_src = htons(54321);
    const struct cls_rule *not_found = dt_lookup_simple(&dt, &flow);
    TEST_RESULT(not_found == NULL, "No match with different source port");
    
    dt_destroy(&dt);
    cls_rule_destroy(rule);
    free(rule);
}

/* Test 5: Priority conflict resolution */
static void
test_priority_conflict(void)
{
    TEST_START("Priority Conflict Resolution");
    
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Insert low-priority catchall rule */
    struct cls_rule *rule_low = xmalloc(sizeof *rule_low);
    struct match match_low;
    match_init_catchall(&match_low);
    cls_rule_init(rule_low, &match_low, 50);
    dt_insert_rule(&dt, rule_low, 1);
    TEST_RESULT(true, "Insert low-priority catchall");
    
    /* Insert high-priority specific rule */
    struct cls_rule *rule_high = xmalloc(sizeof *rule_high);
    struct match match_high;
    match_init_catchall(&match_high);
    match_set_in_port(&match_high, 1);
    cls_rule_init(rule_high, &match_high, 100);
    dt_insert_rule(&dt, rule_high, 1);
    TEST_RESULT(true, "Insert high-priority specific rule");
    
    /* Lookup should match high-priority rule */
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.in_port.ofp_port = 1;
    
    const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
    TEST_RESULT(found != NULL, "Found matching rule");
    TEST_RESULT(found && found->priority == 100, 
                "Matched high-priority rule (not low-priority)");
    
    /* Flow on different port should match catchall */
    flow.in_port.ofp_port = 2;
    found = dt_lookup_simple(&dt, &flow);
    TEST_RESULT(found != NULL, "Found catchall for different port");
    TEST_RESULT(found && found->priority == 50, 
                "Matched low-priority catchall");
    
    dt_destroy(&dt);
    cls_rule_destroy(rule_low);
    cls_rule_destroy(rule_high);
    free(rule_low);
    free(rule_high);
}

/* Test 6: Moderate number of rules (50) */
static void
test_moderate_rules(void)
{
    TEST_START("Moderate Number of Rules (50)");
    
    struct decision_tree dt;
    dt_init(&dt);
    
    const int N_RULES = 50;
    
    /* Insert 50 rules with different IP addresses */
    for (int i = 0; i < N_RULES; i++) {
        struct cls_rule *rule = xmalloc(sizeof *rule);
        struct match match;
        match_init_catchall(&match);
        
        match_set_dl_type(&match, htons(ETH_TYPE_IP));
        match_set_nw_src(&match, htonl(0x0a000000 + i));  /* 10.0.0.0 + i */
        
        cls_rule_init(rule, &match, i);
        
        bool ok = dt_insert_rule(&dt, rule, 1);
        if (!ok) {
            printf("  ! Insert failed at rule %d\n", i);
            TEST_RESULT(false, "Insert all 50 rules");
            dt_destroy(&dt);
            return;
        }
    }
    
    TEST_RESULT(true, "Insert all 50 rules");
    
    /* Get statistics */
    size_t n_rules, n_internal, n_leaf, max_depth;
    dt_get_stats(&dt, &n_rules, &n_internal, &n_leaf, &max_depth);
    
    TEST_RESULT(n_rules == N_RULES, "Correct rule count");
    printf("  Statistics: rules=%zu, internal=%zu, leaf=%zu, depth=%zu\n",
           n_rules, n_internal, n_leaf, max_depth);
    
    /* Test lookup for a few rules */
    for (int i = 0; i < 5; i++) {
        struct flow flow;
        memset(&flow, 0, sizeof flow);
        flow.dl_type = htons(ETH_TYPE_IP);
        flow.nw_src = htonl(0x0a000000 + i * 10);  /* Test rules 0, 10, 20, 30, 40 */
        
        const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
        if (found) {
            printf("  Lookup rule %d: found priority=%d\n", i*10, found->priority);
        }
    }
    
    dt_destroy(&dt);
}

/* Test 7: VLAN filtering */
static void
test_vlan_filtering(void)
{
    TEST_START("VLAN Filtering");
    
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Create rule matching VLAN 100 */
    struct cls_rule *rule = xmalloc(sizeof *rule);
    struct match match;
    match_init_catchall(&match);
    
    match_set_dl_vlan(&match, htons(100), 0);  /* Add required id parameter */
    
    cls_rule_init(rule, &match, 100);
    bool inserted = dt_insert_rule(&dt, rule, 1);
    TEST_RESULT(inserted, "Insert VLAN rule");
    
    /* Create flow with VLAN 100 */
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.vlans[0].tci = htons(VLAN_CFI | 100);
    
    const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
    TEST_RESULT(found != NULL, "Lookup matching VLAN");
    
    /* Test with different VLAN */
    flow.vlans[0].tci = htons(VLAN_CFI | 200);
    const struct cls_rule *not_found = dt_lookup_simple(&dt, &flow);
    TEST_RESULT(not_found == NULL, "No match for different VLAN");
    
    dt_destroy(&dt);
    cls_rule_destroy(rule);
    free(rule);
}

/* Test 8: Protocol filtering (TCP/UDP/ICMP) */
static void
test_protocol_filtering(void)
{
    TEST_START("Protocol Filtering");
    
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Create TCP rule */
    struct cls_rule *tcp_rule = xmalloc(sizeof *tcp_rule);
    struct match tcp_match;
    match_init_catchall(&tcp_match);
    match_set_dl_type(&tcp_match, htons(ETH_TYPE_IP));
    match_set_nw_proto(&tcp_match, IPPROTO_TCP);
    cls_rule_init(tcp_rule, &tcp_match, 100);
    dt_insert_rule(&dt, tcp_rule, 1);
    
    /* Create UDP rule */
    struct cls_rule *udp_rule = xmalloc(sizeof *udp_rule);
    struct match udp_match;
    match_init_catchall(&udp_match);
    match_set_dl_type(&udp_match, htons(ETH_TYPE_IP));
    match_set_nw_proto(&udp_match, IPPROTO_UDP);
    cls_rule_init(udp_rule, &udp_match, 90);
    dt_insert_rule(&dt, udp_rule, 1);
    
    /* Test TCP flow */
    struct flow tcp_flow;
    memset(&tcp_flow, 0, sizeof tcp_flow);
    tcp_flow.dl_type = htons(ETH_TYPE_IP);
    tcp_flow.nw_proto = IPPROTO_TCP;
    
    const struct cls_rule *found = dt_lookup_simple(&dt, &tcp_flow);
    TEST_RESULT(found != NULL && found->priority == 100, "Match TCP rule");
    
    /* Test UDP flow */
    struct flow udp_flow;
    memset(&udp_flow, 0, sizeof udp_flow);
    udp_flow.dl_type = htons(ETH_TYPE_IP);
    udp_flow.nw_proto = IPPROTO_UDP;
    
    found = dt_lookup_simple(&dt, &udp_flow);
    TEST_RESULT(found != NULL && found->priority == 90, "Match UDP rule");
    
    /* Test ICMP flow (no matching rule) */
    struct flow icmp_flow;
    memset(&icmp_flow, 0, sizeof icmp_flow);
    icmp_flow.dl_type = htons(ETH_TYPE_IP);
    icmp_flow.nw_proto = IPPROTO_ICMP;
    
    found = dt_lookup_simple(&dt, &icmp_flow);
    TEST_RESULT(found == NULL, "No match for ICMP");
    
    dt_destroy(&dt);
    cls_rule_destroy(tcp_rule);
    cls_rule_destroy(udp_rule);
    free(tcp_rule);
    free(udp_rule);
}

int
main(void)
{
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║   DT Classifier - Extended Test Suite             ║\n");
    printf("╚════════════════════════════════════════════════════╝\n");
    
    test_mac_address();
    test_ip_address();
    test_tcp_ports();
    test_5tuple_match();
    test_priority_conflict();
    test_moderate_rules();
    test_vlan_filtering();
    test_protocol_filtering();
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║   Test Results Summary                             ║\n");
    printf("╠════════════════════════════════════════════════════╣\n");
    printf("║   Passed: %-3d                                      ║\n", tests_passed);
    printf("║   Failed: %-3d                                      ║\n", tests_failed);
    printf("║   Total:  %-3d                                      ║\n", tests_passed + tests_failed);
    printf("╠════════════════════════════════════════════════════╣\n");
    if (tests_failed == 0) {
        printf("║   Result: ✓ ALL TESTS PASSED                      ║\n");
    } else {
        printf("║   Result: ✗ SOME TESTS FAILED                     ║\n");
    }
    printf("╚════════════════════════════════════════════════════╝\n");
    
    return tests_failed > 0 ? 1 : 0;
}
