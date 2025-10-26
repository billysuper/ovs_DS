/*
 * Copyright (c) 2025 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#undef NDEBUG
#include "dt-classifier.h"
#include "classifier.h"
#include "classifier-private.h"
#include "flow.h"
#include "openvswitch/match.h"
#include "openvswitch/vlog.h"
#include "ovstest.h"
#include "timeval.h"
#include "util.h"
#include <assert.h>

VLOG_DEFINE_THIS_MODULE(test_dt_bulk);

/* Test rule structure */
struct test_rule {
    struct cls_rule cls_rule;
    int id;  /* Rule ID for verification */
};

/* Create a test rule */
static struct test_rule *
create_test_rule(int id, ovs_be32 nw_src, int priority)
{
    struct test_rule *rule = xzalloc(sizeof *rule);
    struct match match;
    
    match_init_catchall(&match);
    match_set_nw_src(&match, nw_src);
    
    cls_rule_init(&rule->cls_rule, &match, priority);
    rule->id = id;
    
    return rule;
}

/* Basic bulk build test */
static void
test_dt_bulk_basic(void)
{
    struct decision_tree dt;
    struct rculist rules_list;
    struct test_rule *rules[100];
    int i;
    int n_test_rules = 10;  /* Start with just 10 rules for debugging */
    
    printf("\n=== Test: DT Bulk Build Basic (%d rules) ===\n", n_test_rules);
    
    /* Initialize */
    dt_init(&dt);
    rculist_init(&rules_list);
    
    /* Create test rules */
    printf("Creating %d test rules...\n", n_test_rules);
    for (i = 0; i < n_test_rules; i++) {
        ovs_be32 ip = htonl(0x0a000000 + i);  /* 10.0.0.0 - 10.0.0.X */
        rules[i] = create_test_rule(i, ip, 100 - i);  /* Decreasing priority */
        
        rculist_push_back(&rules_list, 
                          CONST_CAST(struct rculist *, &rules[i]->cls_rule.node));
    }
    
    /* Build tree */
    printf("Building tree with dt_build_tree()...\n");
    printf("  Rules list addr: %p\n", (void*)&rules_list);
    printf("  Number of rules: %d\n", n_test_rules);
    printf("  Calling dt_build_tree...\n");
    fflush(stdout);
    
    long long start = time_msec();
    
    struct dt_node *root = dt_build_tree(&rules_list, n_test_rules, 10);
    
    long long end = time_msec();
    printf("Tree built in %lld ms\n", end - start);
    
    if (!root) {
        printf("✗ FAILED: Could not build tree!\n");
        goto cleanup;
    }
    
    printf("✓ Tree built successfully\n");
    ovsrcu_set(&dt.root, root);
    dt.n_rules = n_test_rules;
    
    /* Test lookups */
    printf("Testing lookups...\n");
    int correct = 0;
    
    for (i = 0; i < n_test_rules; i++) {
        struct flow flow;
        memset(&flow, 0, sizeof flow);
        flow.nw_src = htonl(0x0a000000 + i);
        
        const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
        
        if (found) {
            struct test_rule *test_rule = 
                CONTAINER_OF(found, struct test_rule, cls_rule);
            
            if (test_rule->id == i) {
                correct++;
            } else {
                printf("✗ Rule %d: expected id=%d, got id=%d\n", 
                       i, i, test_rule->id);
            }
        } else {
            printf("✗ Rule %d: not found!\n", i);
        }
    }
    
    printf("Lookup test: %d/%d correct ", correct, n_test_rules);
    if (correct == n_test_rules) {
        printf("✓ PASS\n");
    } else {
        printf("✗ FAIL\n");
    }
    
    /* Get statistics */
    size_t n_rules, n_internal, n_leaf, max_depth;
    dt_get_stats(&dt, &n_rules, &n_internal, &n_leaf, &max_depth);
    
    printf("Tree statistics:\n");
    printf("  Rules: %zu\n", n_rules);
    printf("  Internal nodes: %zu\n", n_internal);
    printf("  Leaf nodes: %zu\n", n_leaf);
    printf("  Max depth: %zu\n", max_depth);
    
cleanup:
    /* Cleanup */
    dt_destroy(&dt);
    for (i = 0; i < 100; i++) {
        cls_rule_destroy(&rules[i]->cls_rule);
        free(rules[i]);
    }
    
    printf("=== Test completed ===\n");
}

/* Scale test with different sizes */
static void
test_dt_bulk_scale(void)
{
    int sizes[] = {10, 50, 100, 500, 1000};
    int i, j;
    
    printf("\n=== Test: DT Bulk Build Scale ===\n");
    
    for (j = 0; j < ARRAY_SIZE(sizes); j++) {
        int n = sizes[j];
        struct decision_tree dt;
        struct rculist rules_list;
        struct test_rule **rules = xmalloc(n * sizeof *rules);
        
        dt_init(&dt);
        rculist_init(&rules_list);
        
        /* Create rules */
        for (i = 0; i < n; i++) {
            ovs_be32 ip = htonl(0x0a000000 + i);
            rules[i] = create_test_rule(i, ip, n - i);
            rculist_push_back(&rules_list, 
                              CONST_CAST(struct rculist *, &rules[i]->cls_rule.node));
        }
        
        /* Build and time */
        long long start = time_msec();
        struct dt_node *root = dt_build_tree(&rules_list, n, 10);
        long long end = time_msec();
        
        if (root) {
            ovsrcu_set(&dt.root, root);
            dt.n_rules = n;
            
            printf("Size %4d: built in %3lld ms", n, end - start);
            
            size_t n_rules, n_internal, n_leaf, max_depth;
            dt_get_stats(&dt, &n_rules, &n_internal, &n_leaf, &max_depth);
            printf(" - Internal: %3zu, Leaf: %3zu, Depth: %2zu\n", 
                   n_internal, n_leaf, max_depth);
        } else {
            printf("Size %4d: ✗ FAILED to build\n", n);
        }
        
        /* Cleanup */
        dt_destroy(&dt);
        for (i = 0; i < n; i++) {
            cls_rule_destroy(&rules[i]->cls_rule);
            free(rules[i]);
        }
        free(rules);
    }
    
    printf("=== Scale test completed ===\n");
}

/* Lookup performance test */
static void
test_dt_bulk_lookup_perf(void)
{
    struct decision_tree dt;
    struct rculist rules_list;
    struct test_rule *rules[1000];
    int i;
    
    printf("\n=== Test: DT Lookup Performance ===\n");
    
    /* Prepare */
    dt_init(&dt);
    rculist_init(&rules_list);
    
    printf("Creating 1000 rules...\n");
    for (i = 0; i < 1000; i++) {
        ovs_be32 ip = htonl(0x0a000000 + i);
        rules[i] = create_test_rule(i, ip, 1000 - i);
        rculist_push_back(&rules_list, 
                          CONST_CAST(struct rculist *, &rules[i]->cls_rule.node));
    }
    
    printf("Building tree...\n");
    struct dt_node *root = dt_build_tree(&rules_list, 1000, 10);
    if (!root) {
        printf("✗ FAILED to build tree\n");
        goto cleanup;
    }
    
    ovsrcu_set(&dt.root, root);
    dt.n_rules = 1000;
    
    /* Performance test */
    printf("Performing 10000 lookups...\n");
    long long start_us = time_usec();
    
    for (i = 0; i < 10000; i++) {
        struct flow flow;
        memset(&flow, 0, sizeof flow);
        flow.nw_src = htonl(0x0a000000 + (i % 1000));
        
        const struct cls_rule *found = dt_lookup_simple(&dt, &flow);
        (void)found;  /* Ignore result, just test performance */
    }
    
    long long end_us = time_usec();
    long long total_us = end_us - start_us;
    
    printf("10000 lookups in %lld ms (avg %.2f us per lookup)\n", 
           total_us / 1000, (double)total_us / 10000);
    
cleanup:
    /* Cleanup */
    dt_destroy(&dt);
    for (i = 0; i < 1000; i++) {
        cls_rule_destroy(&rules[i]->cls_rule);
        free(rules[i]);
    }
    
    printf("=== Performance test completed ===\n");
}

/* Main test entry point */
static void
test_dt_bulk(int argc OVS_UNUSED, char *argv[] OVS_UNUSED)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   Decision Tree Bulk Build Test Suite       ║\n");
    printf("╚══════════════════════════════════════════════╝\n");
    
    test_dt_bulk_basic();
    test_dt_bulk_scale();
    test_dt_bulk_lookup_perf();
    
    printf("\n");
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   All tests completed                        ║\n");
    printf("╚══════════════════════════════════════════════╝\n");
    printf("\n");
}

OVSTEST_REGISTER("test-dt-bulk", test_dt_bulk);
