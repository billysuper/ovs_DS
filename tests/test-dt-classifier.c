/*
 * Copyright (c) 2025 Decision Tree Classifier Test Suite
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include <config.h>
#undef NDEBUG
#include "dt-classifier.h"
#include <errno.h>
#include <limits.h>
#include "classifier.h"
#include "classifier-private.h"
#include "command-line.h"
#include "flow.h"
#include "openvswitch/ofp-print.h"
#include "ovstest.h"
#include "packets.h"
#include "random.h"
#include "timeval.h"
#include "unaligned.h"
#include "util.h"
#include "openvswitch/vlog.h"

VLOG_DEFINE_THIS_MODULE(test_dt_classifier);

/* ========== Simple Linear Classifier (Reference Implementation) ========== */

/* Simple classifier for verification - just a linear list of rules */
struct dt_simple {
    struct ovs_list rules;  /* List of dt_simple_rule */
    size_t n_rules;
};

struct dt_simple_rule {
    struct ovs_list node;
    struct cls_rule cls_rule;
    unsigned int priority;
};

static void
dt_simple_init(struct dt_simple *simple)
{
    ovs_list_init(&simple->rules);
    simple->n_rules = 0;
}

static void
dt_simple_destroy(struct dt_simple *simple)
{
    struct dt_simple_rule *rule, *next;
    
    LIST_FOR_EACH_SAFE (rule, next, node, &simple->rules) {
        ovs_list_remove(&rule->node);
        cls_rule_destroy(&rule->cls_rule);
        free(rule);
    }
    
    simple->n_rules = 0;
}

static void
dt_simple_insert(struct dt_simple *simple, const struct match *match,
                 unsigned int priority)
{
    struct dt_simple_rule *new_rule = xzalloc(sizeof *new_rule);
    
    cls_rule_init(&new_rule->cls_rule, match, priority);
    new_rule->priority = priority;
    
    /* Insert in priority order (highest first) */
    struct dt_simple_rule *rule;
    LIST_FOR_EACH (rule, node, &simple->rules) {
        if (priority > rule->priority) {
            ovs_list_insert(&rule->node, &new_rule->node);
            simple->n_rules++;
            return;
        }
    }
    
    /* Insert at end if lowest priority or list is empty */
    ovs_list_push_back(&simple->rules, &new_rule->node);
    simple->n_rules++;
}

static const struct cls_rule *
dt_simple_lookup(const struct dt_simple *simple, const struct flow *flow)
{
    struct dt_simple_rule *rule;
    
    /* Linear search - return first matching rule (highest priority) */
    LIST_FOR_EACH (rule, node, &simple->rules) {
        if (minimatch_matches_flow(&rule->cls_rule.match, flow)) {
            return &rule->cls_rule;
        }
    }
    
    return NULL;
}

/* ========== Test Flow Generation ========== */

/* Predefined test values for flow fields */
static const ovs_be32 nw_src_values[] = {
    0,
    CONSTANT_HTONL(0x0a000001),  /* 10.0.0.1 */
    CONSTANT_HTONL(0x0a000002),  /* 10.0.0.2 */
    CONSTANT_HTONL(0xc0a80101),  /* 192.168.1.1 */
    CONSTANT_HTONL(0xc0a80102),  /* 192.168.1.2 */
};

static const ovs_be32 nw_dst_values[] = {
    0,
    CONSTANT_HTONL(0x0a000001),
    CONSTANT_HTONL(0x0a000002),
    CONSTANT_HTONL(0xc0a80101),
    CONSTANT_HTONL(0xc0a80102),
};

static const ovs_be16 tp_src_values[] = {
    0,
    CONSTANT_HTONS(80),
    CONSTANT_HTONS(443),
    CONSTANT_HTONS(8080),
};

static const ovs_be16 tp_dst_values[] = {
    0,
    CONSTANT_HTONS(80),
    CONSTANT_HTONS(443),
    CONSTANT_HTONS(8080),
};

static const uint8_t nw_proto_values[] = {
    0,
    IPPROTO_TCP,
    IPPROTO_UDP,
    IPPROTO_ICMP,
};

static const uint16_t in_port_values[] = {
    0,
    1,
    2,
    3,
};

#define N_NW_SRC_VALUES ARRAY_SIZE(nw_src_values)
#define N_NW_DST_VALUES ARRAY_SIZE(nw_dst_values)
#define N_TP_SRC_VALUES ARRAY_SIZE(tp_src_values)
#define N_TP_DST_VALUES ARRAY_SIZE(tp_dst_values)
#define N_NW_PROTO_VALUES ARRAY_SIZE(nw_proto_values)
#define N_IN_PORT_VALUES ARRAY_SIZE(in_port_values)

/* Total number of possible flow combinations */
#define N_FLOW_VALUES (N_NW_SRC_VALUES * N_NW_DST_VALUES * \
                       N_TP_SRC_VALUES * N_TP_DST_VALUES * \
                       N_NW_PROTO_VALUES * N_IN_PORT_VALUES)

/* Generate test flow from index */
static void
make_test_flow(int idx, struct flow *flow)
{
    memset(flow, 0, sizeof *flow);
    
    /* Decompose index into field values */
    int i = idx;
    
    flow->in_port.ofp_port = in_port_values[i % N_IN_PORT_VALUES];
    i /= N_IN_PORT_VALUES;
    
    flow->nw_proto = nw_proto_values[i % N_NW_PROTO_VALUES];
    i /= N_NW_PROTO_VALUES;
    
    flow->tp_dst = tp_dst_values[i % N_TP_DST_VALUES];
    i /= N_TP_DST_VALUES;
    
    flow->tp_src = tp_src_values[i % N_TP_SRC_VALUES];
    i /= N_TP_SRC_VALUES;
    
    flow->nw_dst = nw_dst_values[i % N_NW_DST_VALUES];
    i /= N_NW_DST_VALUES;
    
    flow->nw_src = nw_src_values[i % N_NW_SRC_VALUES];
    
    /* Set eth_type based on protocol */
    if (flow->nw_proto) {
        flow->dl_type = htons(ETH_TYPE_IP);
    }
}

/* Generate test match with wildcards from index */
static void
make_test_match(int idx, struct match *match, unsigned int *priority)
{
    struct flow flow;
    make_test_flow(idx, &flow);
    
    match_init_catchall(match);
    
    /* Add some wildcarding variation */
    int wildcard_pattern = idx % 8;
    
    if (wildcard_pattern & 1) {
        match_set_nw_src(match, flow.nw_src);
    }
    if (wildcard_pattern & 2) {
        match_set_nw_dst(match, flow.nw_dst);
    }
    if (wildcard_pattern & 4) {
        match_set_nw_proto(match, flow.nw_proto);
        if (flow.nw_proto == IPPROTO_TCP || flow.nw_proto == IPPROTO_UDP) {
            match_set_tp_src(match, flow.tp_src);
            match_set_tp_dst(match, flow.tp_dst);
        }
    }
    
    match_set_in_port(match, flow.in_port.ofp_port);
    
    /* Priority based on specificity */
    *priority = __builtin_popcount(wildcard_pattern) * 1000 + (idx % 100);
}

/* ========== Dual Classifier Verification ========== */

/* Compare results from decision tree and simple classifier */
static bool
compare_dt_classifiers(struct decision_tree *dt, struct dt_simple *simple,
                       size_t n_tests, int *errors)
{
    bool ok = true;
    *errors = 0;
    
    for (size_t i = 0; i < n_tests; i++) {
        struct flow flow;
        make_test_flow(i % N_FLOW_VALUES, &flow);
        
        const struct cls_rule *dt_result = dt_lookup_simple(dt, &flow);
        const struct cls_rule *simple_result = dt_simple_lookup(simple, &flow);
        
        /* Both should return same rule (or both NULL) */
        if (dt_result != simple_result) {
            /* Compare priorities if both found rules */
            if (dt_result && simple_result) {
                if (dt_result->priority != simple_result->priority) {
                    VLOG_ERR("Flow %zu: DT priority=%u, Simple priority=%u",
                             i, dt_result->priority, simple_result->priority);
                    (*errors)++;
                    ok = false;
                }
            } else {
                VLOG_ERR("Flow %zu: DT=%s, Simple=%s",
                         i, dt_result ? "MATCH" : "NULL",
                         simple_result ? "MATCH" : "NULL");
                (*errors)++;
                ok = false;
            }
        }
        
        if (i % 100 == 0 && i > 0) {
            printf(".");
            fflush(stdout);
        }
    }
    
    printf("\n");
    return ok;
}

/* ========== Basic Tests ========== */

static void
test_dt_empty(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Lookup in empty tree should return NULL */
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    
    const struct cls_rule *result = dt_lookup_simple(&dt, &flow);
    ovs_assert(result == NULL);
    
    /* Stats should be zero */
    size_t n_rules, n_internal, n_leaf, max_depth;
    dt_get_stats(&dt, &n_rules, &n_internal, &n_leaf, &max_depth);
    ovs_assert(n_rules == 0);
    
    dt_destroy(&dt);
    printf("PASSED: empty tree test\n");
}

static void
test_dt_single_rule(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Add single rule */
    struct match match;
    match_init_catchall(&match);
    match_set_nw_src(&match, htonl(0x0a000001));  /* 10.0.0.1 */
    
    struct cls_rule rule;
    cls_rule_init(&rule, &match, 100);
    
    dt_add_rule_lazy(&dt, &rule);
    dt_ensure_tree_built(&dt);
    
    /* Test matching flow */
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.nw_src = htonl(0x0a000001);
    
    const struct cls_rule *result = dt_lookup_simple(&dt, &flow);
    ovs_assert(result != NULL);
    ovs_assert(result->priority == 100);
    
    /* Test non-matching flow */
    flow.nw_src = htonl(0x0a000002);
    result = dt_lookup_simple(&dt, &flow);
    ovs_assert(result == NULL);
    
    cls_rule_destroy(&rule);
    dt_destroy(&dt);
    printf("PASSED: single rule test\n");
}

static void
test_dt_priority_ordering(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Add multiple overlapping rules with different priorities */
    struct match match1, match2, match3;
    struct cls_rule rule1, rule2, rule3;
    
    /* Rule 1: Catch-all with priority 10 */
    match_init_catchall(&match1);
    cls_rule_init(&rule1, &match1, 10);
    
    /* Rule 2: Match specific IP with priority 100 */
    match_init_catchall(&match2);
    match_set_nw_src(&match2, htonl(0x0a000001));
    cls_rule_init(&rule2, &match2, 100);
    
    /* Rule 3: Match specific IP+port with priority 1000 */
    match_init_catchall(&match3);
    match_set_nw_src(&match3, htonl(0x0a000001));
    match_set_nw_proto(&match3, IPPROTO_TCP);
    match_set_tp_dst(&match3, htons(80));
    cls_rule_init(&rule3, &match3, 1000);
    
    dt_add_rule_lazy(&dt, &rule1);
    dt_add_rule_lazy(&dt, &rule2);
    dt_add_rule_lazy(&dt, &rule3);
    dt_ensure_tree_built(&dt);
    
    /* Test 1: Specific match should return highest priority rule */
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    flow.nw_src = htonl(0x0a000001);
    flow.nw_proto = IPPROTO_TCP;
    flow.tp_dst = htons(80);
    flow.dl_type = htons(ETH_TYPE_IP);
    
    const struct cls_rule *result = dt_lookup_simple(&dt, &flow);
    ovs_assert(result != NULL);
    ovs_assert(result->priority == 1000);
    
    /* Test 2: Less specific match */
    flow.tp_dst = htons(443);  /* Different port */
    result = dt_lookup_simple(&dt, &flow);
    ovs_assert(result != NULL);
    ovs_assert(result->priority == 100);
    
    /* Test 3: Least specific match */
    flow.nw_src = htonl(0xc0a80101);  /* Different IP */
    result = dt_lookup_simple(&dt, &flow);
    ovs_assert(result != NULL);
    ovs_assert(result->priority == 10);
    
    cls_rule_destroy(&rule1);
    cls_rule_destroy(&rule2);
    cls_rule_destroy(&rule3);
    dt_destroy(&dt);
    printf("PASSED: priority ordering test\n");
}

static void
test_dt_dual_classifier(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct decision_tree dt;
    struct dt_simple simple;
    
    dt_init(&dt);
    dt_simple_init(&simple);
    
    printf("Building dual classifiers with random rules...\n");
    
    /* Add same rules to both classifiers */
    const int n_rules = 50;
    struct cls_rule *rules = xmalloc(n_rules * sizeof *rules);
    
    for (int i = 0; i < n_rules; i++) {
        struct match match;
        unsigned int priority;
        
        make_test_match(i, &match, &priority);
        
        /* Add to decision tree */
        cls_rule_init(&rules[i], &match, priority);
        dt_add_rule_lazy(&dt, &rules[i]);
        
        /* Add to simple classifier */
        dt_simple_insert(&simple, &match, priority);
    }
    
    /* Build decision tree */
    dt_ensure_tree_built(&dt);
    
    printf("Decision tree stats:\n");
    dt_print_tree_info(&dt, "  ");
    
    /* Compare results on test flows */
    printf("Comparing %zu lookups", N_FLOW_VALUES);
    fflush(stdout);
    
    int errors;
    bool ok = compare_dt_classifiers(&dt, &simple, N_FLOW_VALUES, &errors);
    
    if (ok) {
        printf("PASSED: All %zu lookups matched!\n", N_FLOW_VALUES);
    } else {
        printf("FAILED: %d errors out of %zu lookups\n", errors, N_FLOW_VALUES);
        ovs_assert(false);
    }
    
    /* Cleanup */
    for (int i = 0; i < n_rules; i++) {
        cls_rule_destroy(&rules[i]);
    }
    free(rules);
    
    dt_destroy(&dt);
    dt_simple_destroy(&simple);
}

static void
test_dt_many_rules(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct decision_tree dt;
    struct dt_simple simple;
    
    dt_init(&dt);
    dt_simple_init(&simple);
    
    const int n_rules = 200;
    printf("Building classifiers with %d rules...\n", n_rules);
    
    struct cls_rule *rules = xmalloc(n_rules * sizeof *rules);
    
    /* Generate more diverse rules */
    for (int i = 0; i < n_rules; i++) {
        struct match match;
        unsigned int priority;
        
        make_test_match(random_uint32(), &match, &priority);
        priority = random_uint32() % 10000;  /* Random priority */
        
        cls_rule_init(&rules[i], &match, priority);
        dt_add_rule_lazy(&dt, &rules[i]);
        dt_simple_insert(&simple, &match, priority);
    }
    
    /* Build tree */
    long long start = time_msec();
    dt_ensure_tree_built(&dt);
    long long end = time_msec();
    
    printf("Tree built in %lld ms\n", end - start);
    dt_print_tree_info(&dt, "  ");
    
    /* Verify correctness */
    printf("Verifying with %zu lookups", N_FLOW_VALUES);
    fflush(stdout);
    
    int errors;
    bool ok = compare_dt_classifiers(&dt, &simple, N_FLOW_VALUES, &errors);
    
    if (ok) {
        printf("PASSED: All lookups matched!\n");
    } else {
        printf("FAILED: %d errors\n", errors);
        ovs_assert(false);
    }
    
    /* Cleanup */
    for (int i = 0; i < n_rules; i++) {
        cls_rule_destroy(&rules[i]);
    }
    free(rules);
    
    dt_destroy(&dt);
    dt_simple_destroy(&simple);
}

static void
test_dt_benchmark(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct decision_tree dt;
    dt_init(&dt);
    
    const int n_rules = 500;
    printf("Benchmark with %d rules...\n", n_rules);
    
    struct cls_rule *rules = xmalloc(n_rules * sizeof *rules);
    
    /* Add rules */
    for (int i = 0; i < n_rules; i++) {
        struct match match;
        unsigned int priority;
        make_test_match(i, &match, &priority);
        
        cls_rule_init(&rules[i], &match, priority);
        dt_add_rule_lazy(&dt, &rules[i]);
    }
    
    /* Build tree and time it */
    long long build_start = time_msec();
    dt_ensure_tree_built(&dt);
    long long build_end = time_msec();
    
    printf("Build time: %lld ms\n", build_end - build_start);
    dt_print_tree_info(&dt, "  ");
    
    /* Benchmark lookups */
    const int n_lookups = 100000;
    printf("Performing %d lookups...\n", n_lookups);
    
    long long lookup_start = time_msec();
    for (int i = 0; i < n_lookups; i++) {
        struct flow flow;
        make_test_flow(random_uint32() % N_FLOW_VALUES, &flow);
        dt_lookup_simple(&dt, &flow);
    }
    long long lookup_end = time_msec();
    
    long long elapsed = lookup_end - lookup_start;
    printf("Lookup time: %lld ms\n", elapsed);
    printf("Throughput: %.2f lookups/ms\n", (double)n_lookups / elapsed);
    printf("Average: %.2f us/lookup\n", (double)elapsed * 1000 / n_lookups);
    
    /* Cleanup */
    for (int i = 0; i < n_rules; i++) {
        cls_rule_destroy(&rules[i]);
    }
    free(rules);
    dt_destroy(&dt);
    
    printf("PASSED: benchmark completed\n");
}

/* ========== Test Command Registration ========== */

static const struct ovs_cmdl_command commands[] = {
    {"empty", NULL, 0, 0, test_dt_empty, OVS_RO},
    {"single-rule", NULL, 0, 0, test_dt_single_rule, OVS_RO},
    {"priority", NULL, 0, 0, test_dt_priority_ordering, OVS_RO},
    {"dual", NULL, 0, 0, test_dt_dual_classifier, OVS_RO},
    {"many", NULL, 0, 0, test_dt_many_rules, OVS_RO},
    {"benchmark", NULL, 0, 0, test_dt_benchmark, OVS_RO},
    {NULL, NULL, 0, 0, NULL, OVS_RO},
};

static void
test_dt_classifier_main(int argc, char *argv[])
{
    set_program_name(argv[0]);
    
    if (argc == 1) {
        /* Run all tests if no specific test specified */
        printf("\n=== Running Decision Tree Classifier Tests ===\n\n");
        
        struct ovs_cmdl_context ctx = { .argc = 0, .argv = NULL };
        
        test_dt_empty(&ctx);
        test_dt_single_rule(&ctx);
        test_dt_priority_ordering(&ctx);
        test_dt_dual_classifier(&ctx);
        test_dt_many_rules(&ctx);
        test_dt_benchmark(&ctx);
        
        printf("\n=== All Tests Passed ===\n");
    } else {
        /* Run specific test */
        struct ovs_cmdl_context ctx = {
            .argc = argc - 1,
            .argv = argv + 1,
        };
        ovs_cmdl_run_command(&ctx, commands);
    }
}

OVSTEST_REGISTER("test-dt-classifier", test_dt_classifier_main);
