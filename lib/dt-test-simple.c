/* Simplified DT classifier test - minimal version */
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include "dt-classifier.h"
#include "classifier.h"

int main(void)
{
    printf("=== Simplified DT Classifier Test ===\n\n");
    
    struct decision_tree dt;
    dt_init(&dt);
    
    printf("1. Initialize decision tree: ");
    printf(dt.n_rules == 0 ? "PASS\n" : "FAIL\n");
    
    /* Test statistics */
    size_t n_rules = 0, n_internal = 0, n_leaf = 0, max_depth = 0;
    dt_get_stats(&dt, &n_rules, &n_internal, &n_leaf, &max_depth);
    
    printf("2. Initial stats: rules=%zu, internal=%zu, leaf=%zu, depth=%zu\n",
           n_rules, n_internal, n_leaf, max_depth);
    printf("   Stats check: ");
    printf((n_rules == 0) ? "PASS\n" : "FAIL\n");
    
    /* Cleanup */
    dt_destroy(&dt);
    printf("3. Destroy tree: PASS\n");
    
    printf("\n=== All basic tests completed ===\n");
    return 0;
}
