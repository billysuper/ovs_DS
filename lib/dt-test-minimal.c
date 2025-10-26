/* Minimal test for decision tree classifier - tests only basic initialization */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include "dt-classifier.h"

int main(void)
{
    printf("=== Minimal DT Classifier Test ===\n");
    
    /* Test 1: Initialize decision tree */
    printf("1. Initialize decision tree... ");
    struct decision_tree dt;
    dt_init(&dt);
    printf("PASS\n");
    
    /* Test 2: Get initial stats */
    printf("2. Get initial stats... ");
    size_t n_rules, n_internal, n_leaf, max_depth;
    dt_get_stats(&dt, &n_rules, &n_internal, &n_leaf, &max_depth);
    printf("PASS\n");
    printf("   Stats: rules=%zu, internal=%zu, leaf=%zu, depth=%zu\n",
           n_rules, n_internal, n_leaf, max_depth);
    
    /* Test 3: Destroy tree */
    printf("3. Destroy tree... ");
    dt_destroy(&dt);
    printf("PASS\n");
    
    printf("\n=== All tests passed! ===\n");
    return 0;
}
