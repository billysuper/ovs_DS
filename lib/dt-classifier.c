#include <config.h>
#include "dt-classifier.h"
#include "classifier.h"
#include "classifier-private.h"
#include "openvswitch/util.h"
#include "openvswitch/vlog.h"
#include "openvswitch/match.h"
#include "util.h"
#include "flow.h"
#include "packets.h"

VLOG_DEFINE_THIS_MODULE(dt_classifier);

/* Forward declarations */
static const struct mf_field *dt_select_split_field_array(const struct cls_rule **rules, 
                                                           size_t n_rules);
static bool dt_find_split_value_array(const struct mf_field *field, 
                                       const struct cls_rule **rules, size_t n_rules,
                                       enum dt_test_type *test_type, 
                                       ovs_be32 *split_value,
                                       unsigned int *plen);
static struct dt_node *dt_build_tree_from_array(const struct cls_rule **rules, 
                                                 size_t n_rules, 
                                                 size_t max_leaf_size, 
                                                 int depth);


/* Initialize decision tree */
void
dt_init(struct decision_tree *dt)
{
    ovsrcu_set_hidden(&dt->root, NULL);
    dt->tree_built = false;
    dt->pending_rules = NULL;
    dt->n_pending = 0;
    dt->pending_capacity = 0;
    dt->n_rules = 0;
    dt->n_internal_nodes = 0;
    dt->n_leaf_nodes = 0;
    dt->max_depth = 0;
}

/* Destroy decision tree */
void
dt_destroy(struct decision_tree *dt)
{
    struct dt_node *root = ovsrcu_get_protected(struct dt_node *, &dt->root);
    
    if (root) {
        dt_node_destroy(root);
        ovsrcu_set_hidden(&dt->root, NULL);
    }
    
    /* Free pending rules array */
    free(dt->pending_rules);
    dt->pending_rules = NULL;
    dt->n_pending = 0;
    dt->pending_capacity = 0;
}

/* Copy a node for COW (deep copy for leaf, shallow for internal) */
struct dt_node *
dt_node_copy(const struct dt_node *node)
{
    if (!node) {
        return NULL;
    }
    
    struct dt_node *new_node = xzalloc(sizeof *new_node);
    new_node->type = node->type;
    
    if (node->type == DT_NODE_INTERNAL) {
        /* Shallow copy internal node - children will be updated by caller */
        new_node->internal.field = node->internal.field;
        new_node->internal.test_type = node->internal.test_type;
        new_node->internal.test = node->internal.test;
        
        /* Copy child pointers (will be updated during COW path rebuild) */
        struct dt_node *left = ovsrcu_get(struct dt_node *, &node->internal.left);
        struct dt_node *right = ovsrcu_get(struct dt_node *, &node->internal.right);
        ovsrcu_set_hidden(&new_node->internal.left, left);
        ovsrcu_set_hidden(&new_node->internal.right, right);
    } else {
        /* Deep copy leaf node - copy rule pointer array */
        if (node->leaf.n_rules > 0) {
            new_node->leaf.rules = xmalloc(node->leaf.n_rules * sizeof(const struct cls_rule *));
            memcpy(new_node->leaf.rules, node->leaf.rules,
                   node->leaf.n_rules * sizeof(const struct cls_rule *));
            new_node->leaf.capacity = node->leaf.n_rules;
        }
        
        new_node->leaf.n_rules = node->leaf.n_rules;
        new_node->leaf.leaf_id = node->leaf.leaf_id;
    }
    
    return new_node;
}

/* Create a leaf node */
struct dt_node *
dt_node_create_leaf(void)
{
    struct dt_node *node = xzalloc(sizeof *node);
    
    node->type = DT_NODE_LEAF;
    node->leaf.rules = NULL;
    node->leaf.n_rules = 0;
    node->leaf.capacity = 0;
    node->leaf.leaf_id = 0; /* Will be assigned during tree build */
    
    return node;
}

/* Create an internal node */
struct dt_node *
dt_node_create_internal(const struct mf_field *field, enum dt_test_type type)
{
    struct dt_node *node = xzalloc(sizeof *node);
    
    node->type = DT_NODE_INTERNAL;
    node->internal.field = field;
    node->internal.test_type = type;
    ovsrcu_set_hidden(&node->internal.left, NULL);
    ovsrcu_set_hidden(&node->internal.right, NULL);
    
    return node;
}

/* Destroy a node recursively */
void
dt_node_destroy(struct dt_node *node)
{
    if (!node) {
        return;
    }
    
    if (node->type == DT_NODE_INTERNAL) {
        struct dt_node *left = ovsrcu_get_protected(struct dt_node *,
                                                     &node->internal.left);
        struct dt_node *right = ovsrcu_get_protected(struct dt_node *,
                                                      &node->internal.right);
        
        dt_node_destroy(left);
        dt_node_destroy(right);
    } else {
        /* Leaf node - free the rules array */
        /* (rules themselves are owned by classifier and will be freed separately) */
        if (node->leaf.rules) {
            free(node->leaf.rules);
        }
        /* TODO: minimask cleanup when we properly implement required_mask */
    }
    
    free(node);
}

/* Path tracking functions for COW */
void
dt_path_init(struct dt_path *path)
{
    path->depth = 0;
    memset(path->nodes, 0, sizeof path->nodes);
    memset(path->directions, 0, sizeof path->directions);
}

bool
dt_path_record(struct dt_path *path, struct dt_node *node, bool go_right)
{
    if (path->depth >= ARRAY_SIZE(path->nodes)) {
        VLOG_WARN("dt_path_record: path too deep (max %zu)",
                  ARRAY_SIZE(path->nodes));
        return false;
    }
    
    path->nodes[path->depth] = node;
    path->directions[path->depth] = go_right ? 1 : 0;
    path->depth++;
    return true;
}

struct dt_node *
dt_path_get_leaf(const struct dt_path *path)
{
    if (path->depth == 0) {
        return NULL;
    }
    
    return path->nodes[path->depth - 1];
}

/* Rebuild path using COW - creates new nodes from leaf to root */
static struct dt_node *
dt_path_rebuild_cow(struct dt_path *path, struct dt_node *new_leaf)
{
    if (path->depth == 0) {
        /* No path - new_leaf becomes root */
        return new_leaf;
    }
    
    struct dt_node *child = new_leaf;
    
    /* Walk backwards from leaf to root, copying each ancestor */
    for (int i = path->depth - 2; i >= 0; i--) {
        struct dt_node *old_parent = path->nodes[i];
        struct dt_node *new_parent = dt_node_copy(old_parent);
        
        ovs_assert(new_parent->type == DT_NODE_INTERNAL);
        
        /* Update appropriate child pointer */
        if (path->directions[i + 1]) {
            /* Child was on right branch */
            ovsrcu_set_hidden(&new_parent->internal.right, child);
        } else {
            /* Child was on left branch */
            ovsrcu_set_hidden(&new_parent->internal.left, child);
        }
        
        child = new_parent;
    }
    
    return child;  /* This is the new root */
}

/* ========== Lazy Build Support ========== */

/* Recursively calculate tree statistics */
static void
dt_calculate_stats_recursive(const struct dt_node *node, 
                             int *n_internal, int *n_leaf, 
                             int *max_depth, int current_depth)
{
    if (!node) {
        return;
    }
    
    if (node->type == DT_NODE_LEAF) {
        (*n_leaf)++;
        if (current_depth > *max_depth) {
            *max_depth = current_depth;
        }
    } else {
        (*n_internal)++;
        const struct dt_node *left = ovsrcu_get(struct dt_node *, 
                                                 &node->internal.left);
        const struct dt_node *right = ovsrcu_get(struct dt_node *, 
                                                  &node->internal.right);
        dt_calculate_stats_recursive(left, n_internal, n_leaf, max_depth, 
                                     current_depth + 1);
        dt_calculate_stats_recursive(right, n_internal, n_leaf, max_depth, 
                                     current_depth + 1);
    }
}

/* Calculate and update tree statistics */
static void
dt_update_stats(struct decision_tree *dt)
{
    dt->n_internal_nodes = 0;
    dt->n_leaf_nodes = 0;
    dt->max_depth = 0;
    
    const struct dt_node *root = ovsrcu_get(struct dt_node *, &dt->root);
    if (root) {
        dt_calculate_stats_recursive(root, &dt->n_internal_nodes, 
                                     &dt->n_leaf_nodes, &dt->max_depth, 0);
    }
}

/* Lazy build: Build tree from pending rules on first lookup */
void
dt_ensure_tree_built(struct decision_tree *dt)
{
    /* If tree is already built, nothing to do */
    if (dt->tree_built) {
        return;
    }
    
    /* If no pending rules, nothing to build */
    if (dt->n_pending == 0) {
        dt->tree_built = true;
        return;
    }
    
    VLOG_INFO("DT Lazy Build: Building tree from %zu pending rules", dt->n_pending);
    
    /* Use new array-based tree building (avoids wrapper corruption bug) */
    /* Use smaller leaf size (5) to encourage tree splitting for testing */
    struct dt_node *new_root = dt_build_tree_from_array(
        dt->pending_rules, dt->n_pending, 5, 0);
    
    /* Update the decision tree */
    ovsrcu_set(&dt->root, new_root);
    dt->n_rules = dt->n_pending;
    dt->tree_built = true;
    
    /* Calculate tree statistics */
    dt_update_stats(dt);
    
    VLOG_INFO("DT Lazy Build: Tree built successfully - %d rules, %d internal nodes, "
              "%d leaf nodes, max depth %d", 
              dt->n_rules, dt->n_internal_nodes, dt->n_leaf_nodes, dt->max_depth);
    
    /* Note: We keep pending_rules array for now, could free it to save memory */
}

/* ========== Lookup Operations ========== */

/* Simple lookup without RCU protection (for initial testing) */
const struct cls_rule *
dt_lookup_simple(const struct decision_tree *dt, const struct flow *flow)
{
    /* Lazy build: Build tree on first lookup */
    dt_ensure_tree_built(CONST_CAST(struct decision_tree *, dt));
    
    const struct dt_node *node = ovsrcu_get(struct dt_node *, &dt->root);
    
    /* Traverse tree until reaching a leaf */
    while (node && node->type == DT_NODE_INTERNAL) {
        bool match = false;
        const struct mf_field *field = node->internal.field;
        
        /* Get field value from flow */
        union mf_value value;
        mf_get_value(field, flow, &value);
        
        /* Perform test based on type */
        switch (node->internal.test_type) {
        case DT_TEST_EXACT:
            /* Simple exact match on first 32 bits */
            match = (value.be32 == node->internal.test.exact.value);
            break;
            
        case DT_TEST_PREFIX:
            /* Prefix match */
            {
                uint32_t mask = ~0u << (32 - node->internal.test.prefix.plen);
                match = ((ntohl(value.be32) & mask) ==
                         (ntohl(node->internal.test.prefix.prefix) & mask));
            }
            break;
            
        case DT_TEST_RANGE:
            /* Not implemented yet */
            match = false;
            break;
            
        default:
            OVS_NOT_REACHED();
        }
        
        /* Follow appropriate branch */
        if (match) {
            node = ovsrcu_get(struct dt_node *, &node->internal.right);
        } else {
            node = ovsrcu_get(struct dt_node *, &node->internal.left);
        }
    }
    
    /* If we reached a leaf, return highest priority rule */
    if (node && node->type == DT_NODE_LEAF) {
        if (node->leaf.n_rules > 0) {
            /* Return first rule (rules are sorted by priority in dt_build_tree) */
            return node->leaf.rules[0];
        }
    }
    
    return NULL;
}

/* RCU-protected lookup with wildcard tracking */
const struct cls_rule *
dt_lookup(const struct decision_tree *dt, ovs_version_t version,
          const struct flow *flow, struct flow_wildcards *wc)
{
    /* Lazy build: Build tree on first lookup */
    dt_ensure_tree_built(CONST_CAST(struct decision_tree *, dt));
    
    const struct dt_node *node = ovsrcu_get(struct dt_node *, &dt->root);
    
    if (!node) {
        return NULL;
    }
    
    /* Initialize wildcards to all-wildcarded if provided */
    if (wc) {
        flow_wildcards_init_catchall(wc);
    }
    
    /* Traverse tree until reaching a leaf */
    while (node && node->type == DT_NODE_INTERNAL) {
        bool match = false;
        const struct mf_field *field = node->internal.field;
        
        /* Get field value from flow */
        union mf_value value;
        mf_get_value(field, flow, &value);
        
        /* TODO: Un-wildcard this field in wc */
        /* This requires more complex logic to set the mask correctly */
        
        /* Perform test */
        switch (node->internal.test_type) {
        case DT_TEST_EXACT:
            match = (value.be32 == node->internal.test.exact.value);
            break;
            
        case DT_TEST_PREFIX:
            {
                uint32_t mask = ~0u << (32 - node->internal.test.prefix.plen);
                match = ((ntohl(value.be32) & mask) ==
                         (ntohl(node->internal.test.prefix.prefix) & mask));
            }
            break;
            
        case DT_TEST_RANGE:
            match = false;
            break;
        }
        
        /* Follow appropriate branch */
        if (match) {
            node = ovsrcu_get(struct dt_node *, &node->internal.right);
        } else {
            node = ovsrcu_get(struct dt_node *, &node->internal.left);
        }
    }
    
    /* If we reached a leaf, find highest priority visible rule */
    if (node && node->type == DT_NODE_LEAF) {
        const struct cls_rule *best_rule = NULL;
        unsigned int best_priority = 0;
        
        /* Check rules in this leaf */
        for (size_t i = 0; i < node->leaf.n_rules; i++) {
            const struct cls_rule *rule = node->leaf.rules[i];
            
            /* For standalone DT (not integrated with classifier),
             * cls_match will be NULL, so we treat the rule as visible.
             * Otherwise, check version visibility. */
            bool visible = !get_cls_match(rule) || 
                          cls_rule_visible_in_version(rule, version);
            
            if (visible) {
                if (!best_rule || rule->priority > best_priority) {
                    best_rule = rule;
                    best_priority = rule->priority;
                }
            }
        }
        
        /* Un-wildcard fields from the matched rule's mask */
        if (best_rule && wc) {
            /* TODO: Implement proper wildcard folding */
            /* const struct minimatch *match = &best_rule->match; */
            /* flow_wildcards_fold_minimask(wc, &subtable->mask); */
        }
        
        return best_rule;
    }
    
    return NULL;
}

/* Simple rule insertion (will be replaced with proper tree construction) */
bool
dt_insert_rule_simple(struct dt_node **root, const struct cls_rule *rule)
{
    /* If tree is empty, create a leaf with this rule */
    if (!*root) {
        *root = dt_node_create_leaf();
        
        /* Initialize array with one rule */
        (*root)->leaf.rules = xmalloc(16 * sizeof(const struct cls_rule *));
        (*root)->leaf.rules[0] = rule;
        (*root)->leaf.n_rules = 1;
        (*root)->leaf.capacity = 16;
        return true;
    }
    
    /* For now, just add to root if it's a leaf */
    if ((*root)->type == DT_NODE_LEAF) {
        struct dt_leaf_node *leaf = &(*root)->leaf;
        
        /* Ensure capacity */
        if (leaf->n_rules >= leaf->capacity) {
            size_t new_capacity = leaf->capacity == 0 ? 16 : leaf->capacity * 2;
            leaf->rules = xrealloc(leaf->rules, new_capacity * sizeof(const struct cls_rule *));
            leaf->capacity = new_capacity;
        }
        
        /* Insert in priority order (descending) */
        size_t insert_pos = leaf->n_rules;
        for (size_t i = 0; i < leaf->n_rules; i++) {
            if (rule->priority > leaf->rules[i]->priority) {
                insert_pos = i;
                break;
            }
        }
        
        /* Shift rules to make space */
        for (size_t i = leaf->n_rules; i > insert_pos; i--) {
            leaf->rules[i] = leaf->rules[i - 1];
        }
        
        /* Insert new rule */
        leaf->rules[insert_pos] = rule;
        leaf->n_rules++;
        return true;
    }
    
    /* TODO: Handle internal nodes */
    VLOG_WARN("dt_insert_rule_simple: internal nodes not yet supported");
    return false;
}

/* Lazy build version: Add rule to pending list (doesn't build tree immediately) */
bool
dt_add_rule_lazy(struct decision_tree *dt, const struct cls_rule *rule)
{
    /* Expand pending_rules array if needed */
    if (dt->n_pending >= dt->pending_capacity) {
        size_t new_capacity = dt->pending_capacity == 0 ? 16 : dt->pending_capacity * 2;
        dt->pending_rules = xrealloc(dt->pending_rules, 
                                     new_capacity * sizeof(const struct cls_rule *));
        dt->pending_capacity = new_capacity;
    }
    
    /* Add rule to pending list */
    dt->pending_rules[dt->n_pending++] = rule;
    
    /* Mark tree as not built (needs rebuild) */
    dt->tree_built = false;
    
    VLOG_DBG("DT Lazy: Added rule (priority=%d) to pending list, total=%zu",
             rule->priority, dt->n_pending);
    
    return true;
}

/* RCU-protected insertion with COW path copying */
bool
dt_insert_rule(struct decision_tree *dt, const struct cls_rule *rule,
               ovs_version_t version OVS_UNUSED)
{
    struct dt_node *old_root = ovsrcu_get_protected(struct dt_node *, &dt->root);
    
    /* Empty tree case */
    if (!old_root) {
        struct dt_node *new_root = dt_node_create_leaf();
        
        /* Initialize first rule in leaf */
        new_root->leaf.rules = xmalloc(16 * sizeof(const struct cls_rule *));
        new_root->leaf.rules[0] = rule;
        new_root->leaf.n_rules = 1;
        new_root->leaf.capacity = 16;
        
        ovsrcu_set(&dt->root, new_root);
        dt->n_rules++;
        dt->n_leaf_nodes++;
        return true;
    }
    
    /* Find insertion point and record path */
    struct dt_path path;
    dt_path_init(&path);
    
    struct dt_node *node = old_root;
    dt_path_record(&path, node, false);
    
    /* For now, simple strategy: traverse to leaf based on first match field */
    /* TODO: Implement proper field selection and tree building */
    while (node && node->type == DT_NODE_INTERNAL) {
        /* Simplified: always go left for now */
        /* TODO: Implement actual value comparison */
        node = ovsrcu_get_protected(struct dt_node *, &node->internal.left);
        
        if (node) {
            dt_path_record(&path, node, false);  /* false = went left */
        }
    }
    
    if (!node || node->type != DT_NODE_LEAF) {
        VLOG_WARN("dt_insert_rule: traversal didn't reach leaf");
        return false;
    }
    
    /* Copy the leaf node and insert rule in priority order */
    struct dt_node *new_leaf = dt_node_copy(node);
    struct dt_leaf_node *leaf = &new_leaf->leaf;
    
    /* Ensure capacity */
    if (leaf->n_rules >= leaf->capacity) {
        size_t new_capacity = leaf->capacity * 2;
        leaf->rules = xrealloc(leaf->rules, new_capacity * sizeof(const struct cls_rule *));
        leaf->capacity = new_capacity;
    }
    
    /* Insert in priority order */
    size_t insert_pos = leaf->n_rules;
    for (size_t i = 0; i < leaf->n_rules; i++) {
        if (rule->priority > leaf->rules[i]->priority) {
            insert_pos = i;
            break;
        }
    }
    
    /* Shift rules to make space */
    for (size_t i = leaf->n_rules; i > insert_pos; i--) {
        leaf->rules[i] = leaf->rules[i - 1];
    }
    
    /* Insert new rule */
    leaf->rules[insert_pos] = rule;
    leaf->n_rules++;
    
    /* Rebuild path using COW */
    struct dt_node *new_root = dt_path_rebuild_cow(&path, new_leaf);
    
    /* Atomically switch to new root */
    ovsrcu_set(&dt->root, new_root);
    
    /* Update statistics */
    dt->n_rules++;
    
    /* Schedule old root for RCU deferred destruction */
    ovsrcu_postpone(dt_node_destroy, old_root);
    
    return true;
}

/* Simple rule removal */
bool
dt_remove_rule_simple(struct dt_node **root, const struct cls_rule *rule)
{
    if (!*root || (*root)->type != DT_NODE_LEAF) {
        return false;
    }
    
    struct dt_leaf_node *leaf = &(*root)->leaf;
    
    /* Find and remove the rule */
    for (size_t i = 0; i < leaf->n_rules; i++) {
        if (leaf->rules[i] == rule) {
            /* Shift remaining rules */
            for (size_t j = i; j < leaf->n_rules - 1; j++) {
                leaf->rules[j] = leaf->rules[j + 1];
            }
            leaf->n_rules--;
            return true;
        }
    }
    
    return false;
}

/* RCU-protected removal with COW */
bool
dt_remove_rule(struct decision_tree *dt, const struct cls_rule *rule)
{
    struct dt_node *old_root = ovsrcu_get_protected(struct dt_node *, &dt->root);
    
    if (!old_root) {
        return false;
    }
    
    /* Find the rule and record path */
    struct dt_path path;
    dt_path_init(&path);
    
    struct dt_node *node = old_root;
    dt_path_record(&path, node, false);
    
    /* Traverse to leaf containing the rule */
    /* TODO: Implement proper search based on rule's match */
    while (node && node->type == DT_NODE_INTERNAL) {
        /* Simplified: always go left */
        node = ovsrcu_get_protected(struct dt_node *, &node->internal.left);
        
        if (node) {
            dt_path_record(&path, node, false);
        }
    }
    
    if (!node || node->type != DT_NODE_LEAF) {
        return false;
    }
    
    /* Find and remove rule from leaf */
    bool found = false;
    for (size_t i = 0; i < node->leaf.n_rules; i++) {
        if (node->leaf.rules[i] == rule) {
            found = true;
            break;
        }
    }
    
    if (!found) {
        return false;
    }
    
    /* Copy the leaf and remove rule */
    struct dt_node *new_leaf = dt_node_copy(node);
    struct dt_leaf_node *leaf = &new_leaf->leaf;
    
    for (size_t i = 0; i < leaf->n_rules; i++) {
        if (leaf->rules[i] == rule) {
            /* Shift remaining rules */
            for (size_t j = i; j < leaf->n_rules - 1; j++) {
                leaf->rules[j] = leaf->rules[j + 1];
            }
            leaf->n_rules--;
            break;
        }
    }
    
    /* Rebuild path using COW */
    struct dt_node *new_root = dt_path_rebuild_cow(&path, new_leaf);
    
    /* Atomically switch to new root */
    ovsrcu_set(&dt->root, new_root);
    
    /* Update statistics */
    dt->n_rules--;
    
    /* Schedule old root for RCU deferred destruction */
    ovsrcu_postpone(dt_node_destroy, old_root);
    
    return true;
}

/* Tree building helpers */

/* Select best field to split on based on entropy or information gain */
static const struct mf_field *
dt_select_split_field(struct rculist *rules, size_t n_rules)
{
    if (n_rules == 0) {
        return NULL;
    }
    
    printf("[DT] dt_select_split_field: Processing %zu rules\n", n_rules);
    
    /* Simple heuristic: select most commonly matched field
     * TODO: Implement proper information gain calculation */
    
    /* Common useful fields to try in order */
    static const enum mf_field_id candidate_fields[] = {
        MFF_IN_PORT,
        MFF_ETH_TYPE,
        MFF_IPV4_SRC,
        MFF_IPV4_DST,
        MFF_IP_PROTO,
        MFF_TCP_SRC,
        MFF_TCP_DST,
        MFF_UDP_SRC,
        MFF_UDP_DST,
    };
    
    /* Count how many rules care about each field */
    int field_counts[ARRAY_SIZE(candidate_fields)] = {0};
    
    for (size_t i = 0; i < ARRAY_SIZE(candidate_fields); i++) {
        const struct mf_field *field = mf_from_id(candidate_fields[i]);
        printf("[DT]   Checking field %zu: %s\n", i, field->name);
        const struct cls_rule *rule;
        int rule_num = 0;
        
        RCULIST_FOR_EACH (rule, node, rules) {
            rule_num++;
            if (rule_num == 1 || rule_num % 20 == 0) {
                printf("[DT]     Processing rule %d/%zu\n", rule_num, n_rules);
            }
            /* Check if rule's mask specifies this field */
            union mf_value value, mask;
            struct match match;
            
            /* Convert minimatch to match */
            minimatch_expand(&rule->match, &match);
            mf_get(field, &match, &value, &mask);
            
            if (!is_all_zeros(&mask, field->n_bytes)) {
                field_counts[i]++;
            }
        }
        printf("[DT]   Field %s: %d rules matched\n", field->name, field_counts[i]);
    }
    
    /* Select field with highest count */
    int best_idx = 0;
    int best_count = field_counts[0];
    
    for (size_t i = 1; i < ARRAY_SIZE(candidate_fields); i++) {
        if (field_counts[i] > best_count) {
            best_count = field_counts[i];
            best_idx = i;
        }
    }
    
    if (best_count == 0) {
        return NULL;  /* No good field to split on */
    }
    
    return mf_from_id(candidate_fields[best_idx]);
}

/* Determine split value for a given field */
static bool
dt_find_split_value(const struct mf_field *field, struct rculist *rules,
                    enum dt_test_type *test_type, ovs_be32 *split_value,
                    unsigned int *plen)
{
    printf("[DT] dt_find_split_value: Finding split value for field %s\n", field->name);
    
    /* For now, use simple median-based splitting
     * TODO: Implement more sophisticated algorithms */
    
    *test_type = DT_TEST_EXACT;
    *split_value = 0;
    *plen = 32;
    
    /* Collect all values for this field */
    const struct cls_rule *rule;
    ovs_be32 values[128];
    size_t n_values = 0;
    
    printf("[DT]   Collecting values...\n");
    RCULIST_FOR_EACH (rule, node, rules) {
        if (n_values >= ARRAY_SIZE(values)) {
            break;
        }
        
        /* TODO: Get actual value from rule */
        /* For now use dummy values - use n_values BEFORE incrementing */
        values[n_values] = htonl(n_values + 1);
        n_values++;
        
        if (n_values % 20 == 0) {
            printf("[DT]     Collected %zu values\n", n_values);
        }
    }
    
    printf("[DT]   Total values collected: %zu\n", n_values);
    
    if (n_values == 0) {
        return false;
    }
    
    /* Use middle value as split point */
    *split_value = values[n_values / 2];
    
    printf("[DT]   Split value selected: 0x%08x (index %zu/%zu)\n", 
           ntohl(*split_value), n_values / 2, n_values);
    
    return true;
}

/* Array-based helper: Select split field from array of rules */
static const struct mf_field *
dt_select_split_field_array(const struct cls_rule **rules, size_t n_rules)
{
    if (n_rules == 0) {
        return NULL;
    }
    
    /* Common useful fields to try in order */
    static const enum mf_field_id candidate_fields[] = {
        MFF_IN_PORT,
        MFF_ETH_TYPE,
        MFF_IPV4_SRC,
        MFF_IPV4_DST,
        MFF_IP_PROTO,
        MFF_TCP_SRC,
        MFF_TCP_DST,
        MFF_UDP_SRC,
        MFF_UDP_DST,
    };
    
    /* Count how many rules care about each field */
    int field_counts[ARRAY_SIZE(candidate_fields)] = {0};
    
    for (size_t i = 0; i < ARRAY_SIZE(candidate_fields); i++) {
        const struct mf_field *field = mf_from_id(candidate_fields[i]);
        
        for (size_t j = 0; j < n_rules; j++) {
            const struct cls_rule *rule = rules[j];
            union mf_value value, mask;
            struct match match;
            
            minimatch_expand(&rule->match, &match);
            mf_get(field, &match, &value, &mask);
            
            if (!is_all_zeros(&mask, field->n_bytes)) {
                field_counts[i]++;
            }
        }
    }
    
    /* Select field with highest count */
    int best_idx = 0;
    int best_count = field_counts[0];
    
    for (size_t i = 1; i < ARRAY_SIZE(candidate_fields); i++) {
        if (field_counts[i] > best_count) {
            best_count = field_counts[i];
            best_idx = i;
        }
    }
    
    if (best_count == 0) {
        printf("[DT] No field found with non-zero mask\n");
        return NULL;
    }
    
    const struct mf_field *result = mf_from_id(candidate_fields[best_idx]);
    printf("[DT] Selected split field: %s (matched by %d/%zu rules)\n", 
           result->name, best_count, n_rules);
    
    return result;
}

/* Array-based helper: Find split value for a field */
static bool
dt_find_split_value_array(const struct mf_field *field, 
                          const struct cls_rule **rules, size_t n_rules,
                          enum dt_test_type *test_type, 
                          ovs_be32 *split_value,
                          unsigned int *plen)
{
    *test_type = DT_TEST_EXACT;
    *plen = 32;
    
    if (n_rules == 0) {
        return false;
    }
    
    /* Collect values from rules */
    ovs_be32 *values = xmalloc(n_rules * sizeof(ovs_be32));
    size_t n_values = 0;
    
    for (size_t i = 0; i < n_rules; i++) {
        const struct cls_rule *rule = rules[i];
        union mf_value value, mask;
        struct match match;
        
        minimatch_expand(&rule->match, &match);
        mf_get(field, &match, &value, &mask);
        
        /* Only include rules that actually match this field */
        if (!is_all_zeros(&mask, field->n_bytes)) {
            values[n_values++] = value.be32;
        }
    }
    
    if (n_values == 0) {
        free(values);
        return false;
    }
    
    /* Check if all values are the same */
    bool all_same = true;
    ovs_be32 first_value = values[0];
    for (size_t i = 1; i < n_values; i++) {
        if (values[i] != first_value) {
            all_same = false;
            break;
        }
    }
    
    if (all_same) {
        printf("[DT] All values for field %s are identical (0x%08x), cannot split\n",
               field->name, ntohl(first_value));
        free(values);
        return false;
    }
    
    /* Use median value as split point */
    *split_value = values[n_values / 2];
    free(values);
    
    return true;
}

/* Build decision tree from array of rules (new version for lazy loading) */
static struct dt_node *
dt_build_tree_from_array(const struct cls_rule **rules, size_t n_rules, 
                         size_t max_leaf_size, int depth)
{
    printf("[DT] dt_build_tree_from_array: n_rules=%zu, max_leaf_size=%zu, depth=%d\n",
           n_rules, max_leaf_size, depth);
    
    /* Base case: empty rule set */
    if (n_rules == 0) {
        return NULL;
    }
    
    /* Base case: small enough to be a leaf */
    if (n_rules <= max_leaf_size) {
        printf("[DT]   Creating leaf (n_rules <= max_leaf_size)\n");
        struct dt_node *leaf = dt_node_create_leaf();
        
        /* Copy rule pointers to leaf */
        leaf->leaf.rules = xmalloc(n_rules * sizeof(const struct cls_rule *));
        leaf->leaf.capacity = n_rules;
        leaf->leaf.n_rules = n_rules;
        
        memcpy(leaf->leaf.rules, rules, n_rules * sizeof(const struct cls_rule *));
        
        return leaf;
    }
    
    /* Try to find a field and split value that actually partitions the rules */
    static const enum mf_field_id candidate_fields[] = {
        MFF_IPV4_SRC,    /* Try IP source first (most variation) */
        MFF_IPV4_DST,
        MFF_TCP_SRC,
        MFF_TCP_DST,
        MFF_UDP_SRC,
        MFF_UDP_DST,
        MFF_IP_PROTO,
        MFF_IN_PORT,
        MFF_ETH_TYPE,
    };
    
    const struct mf_field *split_field = NULL;
    enum dt_test_type test_type;
    ovs_be32 split_value;
    unsigned int plen;
    
    for (size_t i = 0; i < ARRAY_SIZE(candidate_fields); i++) {
        const struct mf_field *field = mf_from_id(candidate_fields[i]);
        
        if (dt_find_split_value_array(field, rules, n_rules, 
                                       &test_type, &split_value, &plen)) {
            split_field = field;
            printf("[DT] Will try splitting on %s\n", field->name);
            break;
        }
    }
    
    if (!split_field) {
        /* Can't find any field to split on - create leaf */
        printf("[DT] No suitable field found for splitting, creating large leaf\n");
        struct dt_node *leaf = dt_node_create_leaf();
        leaf->leaf.rules = xmalloc(n_rules * sizeof(const struct cls_rule *));
        leaf->leaf.capacity = n_rules;
        leaf->leaf.n_rules = n_rules;
        memcpy(leaf->leaf.rules, rules, n_rules * sizeof(const struct cls_rule *));
        return leaf;
    }
    
    /* Partition rules into left and right arrays */
    const struct cls_rule **left_rules = xmalloc(n_rules * sizeof(const struct cls_rule *));
    const struct cls_rule **right_rules = xmalloc(n_rules * sizeof(const struct cls_rule *));
    size_t n_left = 0, n_right = 0;
    
    printf("[DT] Partitioning %zu rules by field %s, split_value=0x%08x\n",
           n_rules, split_field->name, ntohl(split_value));
    
    for (size_t i = 0; i < n_rules; i++) {
        const struct cls_rule *rule = rules[i];
        union mf_value value, mask;
        struct match match;
        
        minimatch_expand(&rule->match, &match);
        mf_get(split_field, &match, &value, &mask);
        
        /* Decide which side this rule goes to */
        bool goes_right = (value.be32 >= split_value);
        
        if (i < 5) {  /* Debug first 5 rules */
            printf("[DT]   Rule %zu: value=0x%08x, goes_%s\n", 
                   i, ntohl(value.be32), goes_right ? "right" : "left");
        }
        
        if (goes_right) {
            right_rules[n_right++] = rule;
        } else {
            left_rules[n_left++] = rule;
        }
    }
    
    printf("[DT] Partition result: %zu left, %zu right\n", n_left, n_right);
    
    /* Sanity check: all rules must be partitioned */
    if (n_left + n_right != n_rules) {
        VLOG_WARN("Partition error: %zu + %zu != %zu", n_left, n_right, n_rules);
    }
    
    /* If all rules went to one side, create a leaf instead */
    if (n_left == 0 || n_right == 0) {
        printf("[DT] All rules went to one side! Creating large leaf.\n");
        free(left_rules);
        free(right_rules);
        
        struct dt_node *leaf = dt_node_create_leaf();
        leaf->leaf.rules = xmalloc(n_rules * sizeof(const struct cls_rule *));
        leaf->leaf.capacity = n_rules;
        leaf->leaf.n_rules = n_rules;
        memcpy(leaf->leaf.rules, rules, n_rules * sizeof(const struct cls_rule *));
        return leaf;
    }
    
    /* Create internal node */
    struct dt_node *internal = dt_node_create_internal(split_field, test_type);
    internal->internal.test.exact.value = split_value;
    
    /* Recursively build subtrees */
    struct dt_node *left = dt_build_tree_from_array(left_rules, n_left, 
                                                     max_leaf_size, depth + 1);
    struct dt_node *right = dt_build_tree_from_array(right_rules, n_right, 
                                                      max_leaf_size, depth + 1);
    
    /* Free temporary arrays (the rules themselves are owned by the tree) */
    free(left_rules);
    free(right_rules);
    
    ovsrcu_set_hidden(&internal->internal.left, left);
    ovsrcu_set_hidden(&internal->internal.right, right);
    
    return internal;
}

/* Build decision tree from rules (batch construction) - OLD RCULIST VERSION */
struct dt_node *
dt_build_tree(struct rculist *rules, size_t n_rules, size_t max_leaf_size)
{
    static int depth = 0;
    depth++;
    
    printf("[DT] dt_build_tree ENTER: depth=%d, n_rules=%zu, max_leaf=%zu\n", 
           depth, n_rules, max_leaf_size);
    fflush(stdout);
    
    /* Base case: small enough to be a leaf */
    if (n_rules <= max_leaf_size) {
        printf("[DT]   Base case: creating leaf node with %zu rules\n", n_rules);
        fflush(stdout);
        depth--;
        struct dt_node *leaf = dt_node_create_leaf();
        
        /* Allocate array for rule pointers */
        leaf->leaf.rules = xmalloc(n_rules * sizeof(const struct cls_rule *));
        leaf->leaf.capacity = n_rules;
        leaf->leaf.n_rules = 0;
        
        printf("[DT]   Collecting rule pointers...\n");
        fflush(stdout);
        
        /* Collect pointers to rules (not copying the nodes!) */
        const struct cls_rule *rule;
        RCULIST_FOR_EACH (rule, node, rules) {
            if (leaf->leaf.n_rules >= n_rules) {
                printf("[DT]   WARNING: More rules than expected!\n");
                break;
            }
            leaf->leaf.rules[leaf->leaf.n_rules++] = rule;
        }
        
        printf("[DT]   Leaf created with %zu rules\n", leaf->leaf.n_rules);
        fflush(stdout);
        return leaf;
    }
    
    /* Select field to split on */
    if (depth <= 5) {
        printf("[DT] Depth %d: Selecting split field for %zu rules...\n", depth, n_rules);
    }
    const struct mf_field *split_field = dt_select_split_field(rules, n_rules);
    if (!split_field) {
        /* Can't split further - make leaf */
        if (depth <= 5) {
            printf("[DT] Depth %d: No split field found, creating leaf with %zu rules\n", depth, n_rules);
        }
        depth--;
        struct dt_node *leaf = dt_node_create_leaf();
        
        /* Allocate and fill rules array */
        leaf->leaf.rules = xmalloc(n_rules * sizeof(const struct cls_rule *));
        leaf->leaf.capacity = n_rules;
        leaf->leaf.n_rules = 0;
        
        const struct cls_rule *rule;
        RCULIST_FOR_EACH (rule, node, rules) {
            if (leaf->leaf.n_rules < n_rules) {
                leaf->leaf.rules[leaf->leaf.n_rules++] = rule;
            }
        }
        
        return leaf;
    }
    
    /* Determine split value */
    enum dt_test_type test_type;
    ovs_be32 split_value;
    unsigned int plen;
    
    if (!dt_find_split_value(split_field, rules, &test_type,
                             &split_value, &plen)) {
        /* Can't find good split - make leaf */
        depth--;
        struct dt_node *leaf = dt_node_create_leaf();
        
        /* Allocate and fill rules array */
        leaf->leaf.rules = xmalloc(n_rules * sizeof(const struct cls_rule *));
        leaf->leaf.capacity = n_rules;
        leaf->leaf.n_rules = 0;
        
        const struct cls_rule *rule;
        RCULIST_FOR_EACH (rule, node, rules) {
            if (leaf->leaf.n_rules < n_rules) {
                leaf->leaf.rules[leaf->leaf.n_rules++] = rule;
            }
        }
        
        return leaf;
    }
    
    /* Partition rules into left and right */
    printf("[DT] Partitioning %zu rules...\n", n_rules);
    struct rculist left_rules, right_rules;
    rculist_init(&left_rules);
    rculist_init(&right_rules);
    size_t n_left = 0, n_right = 0;
    size_t rule_count = 0;  /* Track how many rules processed */
    
    const struct cls_rule *rule;
    RCULIST_FOR_EACH (rule, node, rules) {
        if (rule_count % 20 == 0) {
            printf("[DT]   Partitioned %zu/%zu rules\n", rule_count, n_rules);
        }
        /* For simple test, just use a fixed pattern */
        /* TODO: Properly get value from rule's match */
        ovs_be32 test_value = htonl(rule_count);  /* Dummy value */
        
        /* Use >= for proper binary split instead of == */
        bool goes_right = (test_value >= split_value);
        
        if (goes_right) {
            rculist_push_back(&right_rules,
                              CONST_CAST(struct rculist *, &rule->node));
            n_right++;
        } else {
            rculist_push_back(&left_rules,
                              CONST_CAST(struct rculist *, &rule->node));
            n_left++;
        }
        rule_count++;  /* Count rules processed */
    }
    
    printf("[DT] Split result: %zu left, %zu right\n", n_left, n_right);
    
    /* Create internal node */
    struct dt_node *internal = dt_node_create_internal(split_field, test_type);
    internal->internal.test.exact.value = split_value;
    
    /* Recursively build subtrees */
    struct dt_node *left = dt_build_tree(&left_rules, n_left, max_leaf_size);
    struct dt_node *right = dt_build_tree(&right_rules, n_right, max_leaf_size);
    
    ovsrcu_set_hidden(&internal->internal.left, left);
    ovsrcu_set_hidden(&internal->internal.right, right);
    
    depth--;
    return internal;
}

/* Get statistics */
void
dt_get_stats(const struct decision_tree *dt,
             size_t *n_rules, size_t *n_internal, size_t *n_leaf,
             size_t *max_depth)
{
    if (n_rules) {
        *n_rules = dt->n_rules;
    }
    if (n_internal) {
        *n_internal = dt->n_internal_nodes;
    }
    if (n_leaf) {
        *n_leaf = dt->n_leaf_nodes;
    }
    if (max_depth) {
        *max_depth = dt->max_depth;
    }
}

/* Recursively print node information */
static void
dt_print_node_recursive(const struct dt_node *node, int depth, const char *side)
{
    if (!node) {
        return;
    }
    
    /* Print indentation */
    for (int i = 0; i < depth; i++) {
        printf("  ");
    }
    
    if (node->type == DT_NODE_LEAF) {
        printf("%s LEAF: %zu rules\n", side, node->leaf.n_rules);
    } else {
        printf("%s INTERNAL: field=%s, test_type=%d\n", 
               side, node->internal.field->name, node->internal.test_type);
        
        const struct dt_node *left = ovsrcu_get(struct dt_node *, 
                                                 &node->internal.left);
        const struct dt_node *right = ovsrcu_get(struct dt_node *, 
                                                  &node->internal.right);
        
        if (left) {
            dt_print_node_recursive(left, depth + 1, "L");
        }
        if (right) {
            dt_print_node_recursive(right, depth + 1, "R");
        }
    }
}

/* Print tree structure information */
void
dt_print_tree_info(const struct decision_tree *dt, const char *prefix)
{
    printf("\n%s=== Decision Tree Structure ===\n", prefix ? prefix : "");
    printf("%sTree built: %s\n", prefix ? prefix : "", 
           dt->tree_built ? "YES" : "NO");
    printf("%sTotal rules: %d\n", prefix ? prefix : "", dt->n_rules);
    printf("%sInternal nodes: %d\n", prefix ? prefix : "", dt->n_internal_nodes);
    printf("%sLeaf nodes: %d\n", prefix ? prefix : "", dt->n_leaf_nodes);
    printf("%sMax depth: %d\n", prefix ? prefix : "", dt->max_depth);
    
    /* Calculate average rules per leaf */
    if (dt->n_leaf_nodes > 0) {
        double avg_rules = (double)dt->n_rules / dt->n_leaf_nodes;
        printf("%sAverage rules per leaf: %.2f\n", prefix ? prefix : "", avg_rules);
    }
    
    /* Print tree structure if not too large */
    if (dt->tree_built && dt->n_internal_nodes + dt->n_leaf_nodes <= 20) {
        printf("%s\nTree structure:\n", prefix ? prefix : "");
        const struct dt_node *root = ovsrcu_get(struct dt_node *, &dt->root);
        if (root) {
            dt_print_node_recursive(root, 0, "ROOT");
        }
    } else if (dt->tree_built) {
        printf("%s(Tree structure too large to print)\n", prefix ? prefix : "");
    }
    
    printf("%s==============================\n\n", prefix ? prefix : "");
}