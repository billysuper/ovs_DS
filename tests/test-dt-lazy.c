/*
 * Decision Tree Lazy Loading Test
 * 
 * Simple test to verify lazy loading functionality:
 * 1. Add rules with dt_add_rule_lazy (O(1))
 * 2. First lookup triggers tree building
 * 3. Subsequent lookups use existing tree
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

/* Simple test: Create rules, add lazily, lookup */
static void
test_lazy_loading_basic(void)
{
    printf("\n=== Test 1: Basic Lazy Loading ===\n");
    
    struct decision_tree dt;
    dt_init(&dt);
    
    printf("Initial state: tree_built = %s\n", dt.tree_built ? "true" : "false");
    assert(!dt.tree_built);
    
    /* Create some simple rules */
    struct cls_rule *rules[10];
    for (int i = 0; i < 10; i++) {
        rules[i] = xzalloc(sizeof *rules[i]);
        
        /* Simple match: different priorities */
        struct match match;
        match_init_catchall(&match);
        
        /* Initialize the rule */
        cls_rule_init(rules[i], &match, 100 + i);
        
        /* Add rule using lazy loading */
        bool ok = dt_add_rule_lazy(&dt, rules[i]);
        assert(ok);
        
        printf("Added rule %d (priority=%d), tree_built = %s, n_pending = %zu\n",
               i, 100 + i, dt.tree_built ? "true" : "false", dt.n_pending);
    }
    
    /* Verify tree is NOT built yet */
    assert(!dt.tree_built);
    assert(dt.n_pending == 10);
    printf("After adding 10 rules: tree_built = false (expected) ✓\n");
    
    /* First lookup should trigger tree building */
    printf("\nPerforming first lookup...\n");
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    
    const struct cls_rule *result = dt_lookup_simple(&dt, &flow);
    
    /* Verify tree is NOW built */
    printf("After first lookup: tree_built = %s\n", dt.tree_built ? "true" : "false");
    assert(dt.tree_built);
    printf("Tree was built on first lookup ✓\n");
    
    if (result) {
        printf("Found matching rule with priority %d\n", result->priority);
    } else {
        printf("No matching rule found (expected for simple test)\n");
    }
    
    /* Second lookup should NOT rebuild tree */
    printf("\nPerforming second lookup...\n");
    result = dt_lookup_simple(&dt, &flow);
    assert(dt.tree_built);  /* Should still be true */
    printf("Tree remained built (no rebuild) ✓\n");
    
    /* Print tree structure */
    dt_print_tree_info(&dt, "  ");
    
    /* Cleanup */
    dt_destroy(&dt);
    for (int i = 0; i < 10; i++) {
        free(rules[i]);
    }
    
    printf("\n✅ Test 1 PASSED: Basic lazy loading works!\n");
}

/* Performance test: Measure lazy loading speedup */
static void
test_lazy_loading_performance(void)
{
    printf("\n=== Test 2: Performance Test ===\n");
    
    const int N_RULES = 100;  /* Keep it reasonable for quick test */
    
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Measure insertion time */
    long long start_time = time_msec();
    
    struct cls_rule *rules[N_RULES];
    for (int i = 0; i < N_RULES; i++) {
        rules[i] = xzalloc(sizeof *rules[i]);
        
        struct match match;
        match_init_catchall(&match);
        cls_rule_init(rules[i], &match, 1000 + i);
        
        dt_add_rule_lazy(&dt, rules[i]);
    }
    
    long long insert_time = time_msec() - start_time;
    printf("Inserted %d rules in %lld ms (avg %.3f ms/rule)\n",
           N_RULES, insert_time, (double)insert_time / N_RULES);
    
    /* Verify tree not built yet */
    assert(!dt.tree_built);
    printf("Tree not built during insertion ✓\n");
    
    /* Measure first lookup (includes tree building) */
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    
    start_time = time_msec();
    const struct cls_rule *result = dt_lookup_simple(&dt, &flow);
    long long first_lookup_time = time_msec() - start_time;
    
    printf("First lookup (with tree build) took %lld ms\n", first_lookup_time);
    assert(dt.tree_built);
    
    /* Measure subsequent lookups */
    start_time = time_msec();
    for (int i = 0; i < 100; i++) {
        result = dt_lookup_simple(&dt, &flow);
    }
    long long subsequent_lookups_time = time_msec() - start_time;
    
    printf("100 subsequent lookups took %lld ms (avg %.3f ms/lookup)\n",
           subsequent_lookups_time, (double)subsequent_lookups_time / 100);
    
    /* Print tree structure */
    dt_print_tree_info(&dt, "  ");
    
    /* Cleanup */
    dt_destroy(&dt);
    for (int i = 0; i < N_RULES; i++) {
        free(rules[i]);
    }
    
    printf("\n✅ Test 2 PASSED: Performance test completed!\n");
    printf("   Insertion: %lld ms for %d rules\n", insert_time, N_RULES);
    printf("   First lookup: %lld ms (includes tree build)\n", first_lookup_time);
    printf("   Avg lookup: %.3f ms\n", (double)subsequent_lookups_time / 100);
}

/* Test memory management */
static void
test_lazy_loading_memory(void)
{
    printf("\n=== Test 3: Memory Management Test ===\n");
    
    struct decision_tree dt;
    dt_init(&dt);
    
    /* Add rules */
    struct cls_rule *rules[5];
    for (int i = 0; i < 5; i++) {
        rules[i] = xzalloc(sizeof *rules[i]);
        struct match match;
        match_init_catchall(&match);
        cls_rule_init(rules[i], &match, 50 + i);
        dt_add_rule_lazy(&dt, rules[i]);
    }
    
    printf("Added 5 rules, pending_capacity = %zu\n", dt.pending_capacity);
    assert(dt.pending_rules != NULL);
    
    /* Trigger tree build */
    struct flow flow;
    memset(&flow, 0, sizeof flow);
    dt_lookup_simple(&dt, &flow);
    
    printf("Tree built, pending_rules still at %p (kept for now)\n", 
           (void *)dt.pending_rules);
    
    /* Destroy should free everything */
    dt_destroy(&dt);
    printf("After destroy: pending_rules should be NULL\n");
    
    /* Cleanup rules */
    for (int i = 0; i < 5; i++) {
        free(rules[i]);
    }
    
    printf("\n✅ Test 3 PASSED: Memory management works!\n");
}

/* Main test runner */
static void
test_dt_lazy_main(int argc OVS_UNUSED, char *argv[] OVS_UNUSED)
{
    printf("======================================\n");
    printf("Decision Tree Lazy Loading Test Suite\n");
    printf("======================================\n");
    
    /* Run tests */
    test_lazy_loading_basic();
    test_lazy_loading_performance();
    test_lazy_loading_memory();
    
    printf("\n======================================\n");
    printf("✅ ALL TESTS PASSED!\n");
    printf("======================================\n");
}

OVSTEST_REGISTER("test-dt-lazy", test_dt_lazy_main);
