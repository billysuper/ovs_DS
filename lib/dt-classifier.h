#ifndef DT_CLASSIFIER_H
#define DT_CLASSIFIER_H 1

#include "flow.h"
#include "openvswitch/match.h"
#include "rculist.h"
#include "ovs-rcu.h"
#include "pvector.h"
#include "versions.h"

/* Decision Tree Node Types */
enum dt_node_type {
    DT_NODE_INTERNAL,  /* Internal node with test condition */
    DT_NODE_LEAF       /* Leaf node containing rules */
};

/* Test type for internal nodes */
enum dt_test_type {
    DT_TEST_EXACT,     /* Exact match on field value */
    DT_TEST_PREFIX,    /* Prefix match (for IP addresses) */
    DT_TEST_RANGE      /* Range match (future extension) */
};

/* Internal node structure */
struct dt_internal_node {
    const struct mf_field *field;  /* Field to test */
    enum dt_test_type test_type;   /* Type of test */
    
    union {
        /* Exact match */
        struct {
            ovs_be32 value;  /* Value to match against */
        } exact;
        
        /* Prefix match */
        struct {
            ovs_be32 prefix;      /* Prefix value */
            unsigned int plen;    /* Prefix length in bits */
        } prefix;
        
        /* Range match (reserved for future) */
        struct {
            ovs_be32 min;
            ovs_be32 max;
        } range;
    } test;
    
    /* Child nodes */
    OVSRCU_TYPE(struct dt_node *) left;   /* False branch */
    OVSRCU_TYPE(struct dt_node *) right;  /* True branch */
};

/* Leaf node structure */
struct dt_leaf_node {
    const struct cls_rule **rules;  /* Array of pointers to rules */
    size_t n_rules;                 /* Number of rules in this leaf */
    size_t capacity;                /* Allocated capacity */
    uint32_t leaf_id;               /* Unique leaf identifier (for debugging) */
};

/* Decision tree node (union of internal and leaf) */
struct dt_node {
    enum dt_node_type type;
    
    union {
        struct dt_internal_node internal;
        struct dt_leaf_node leaf;
    };
};

/* Decision tree structure */
struct decision_tree {
    OVSRCU_TYPE(struct dt_node *) root;  /* Root of the tree (published version) */
    
    /* Defer/publish support */
    bool publish;                 /* If true, publish changes immediately */
    OVSRCU_TYPE(struct dt_node *) temp_root;  /* Temporary root for deferred changes */
    int defer_depth;              /* Nesting depth (for error detection) */
    
    /* Lazy build support */
    bool tree_built;              /* Whether tree has been built */
    const struct cls_rule **pending_rules;  /* Rules waiting to be built into tree */
    size_t n_pending;             /* Number of pending rules */
    size_t pending_capacity;      /* Capacity of pending_rules array */
    
    /* Statistics (protected by external mutex during modifications) */
    int n_rules;                  /* Total number of rules */
    int n_internal_nodes;         /* Number of internal nodes */
    int n_leaf_nodes;             /* Number of leaf nodes */
    int max_depth;                /* Maximum depth of the tree */
};

/* Decision tree configuration */
struct dt_build_config {
    enum {
        DT_BUILD_SIMPLE,      /* Simple sequential insertion */
        DT_BUILD_GREEDY,      /* Greedy field selection */
        DT_BUILD_BALANCED     /* Balanced tree construction */
    } strategy;
    
    size_t leaf_threshold;        /* Maximum rules per leaf before split */
};

/* Basic operations */
void dt_init(struct decision_tree *dt);
void dt_destroy(struct decision_tree *dt);

/* Node creation/destruction */
struct dt_node *dt_node_create_leaf(void);
struct dt_node *dt_node_create_internal(const struct mf_field *field,
                                        enum dt_test_type type);
void dt_node_destroy(struct dt_node *node);

/* Node copying for COW (Copy-On-Write) */
struct dt_node *dt_node_copy(const struct dt_node *node);
struct dt_node *dt_node_copy_shallow(const struct dt_node *node);

/* Path recording for COW updates */
struct dt_path {
    struct dt_node *nodes[64];  /* Path from root to target */
    uint8_t directions[64];     /* 0 = left, 1 = right */
    size_t depth;               /* Current depth */
};

void dt_path_init(struct dt_path *path);
bool dt_path_record(struct dt_path *path, struct dt_node *node, bool go_right);
struct dt_node *dt_path_get_leaf(const struct dt_path *path);

/* Simple lookup (single-threaded, no RCU yet) */
const struct cls_rule *
dt_lookup_simple(const struct decision_tree *dt, const struct flow *flow);

/* RCU-protected lookup with wildcard tracking */
const struct cls_rule *
dt_lookup(const struct decision_tree *dt, ovs_version_t version,
          const struct flow *flow, struct flow_wildcards *wc);

/* Rule insertion/removal (simple version) */
bool dt_insert_rule_simple(struct dt_node **root, const struct cls_rule *rule);
bool dt_remove_rule_simple(struct dt_node **root, const struct cls_rule *rule);

/* Lazy build: Add rule to pending list (doesn't build tree) */
/* This should ONLY be used during initialization before tree is built */
bool dt_add_rule_lazy(struct decision_tree *dt, const struct cls_rule *rule);

/* RCU-protected insertion/removal with COW */
/* Automatically uses lazy insertion before tree is built, COW after */
bool dt_insert_rule(struct decision_tree *dt, const struct cls_rule *rule,
                    ovs_version_t version);
bool dt_remove_rule(struct decision_tree *dt, const struct cls_rule *rule);

/* Rule replacement (returns displaced rule if any) */
const struct cls_rule *
dt_replace_rule(struct decision_tree *dt, const struct cls_rule *rule,
                ovs_version_t version);

/* Find exact rule match */
const struct cls_rule *
dt_find_rule_exactly(const struct decision_tree *dt,
                     const struct cls_rule *target,
                     ovs_version_t version);

const struct cls_rule *
dt_find_match_exactly(const struct decision_tree *dt,
                      const struct match *target,
                      int priority,
                      ovs_version_t version);

/* Iterator support */
struct dt_cursor {
    const struct decision_tree *dt;
    ovs_version_t version;              /* Version to iterate */
    const struct cls_rule *target;      /* Target for filtering (NULL = all) */
    
    /* Depth-first traversal stack */
    struct dt_node *stack[64];          /* Node stack (max depth 64) */
    int directions[64];                 /* 0=left, 1=right, 2=done */
    int depth;                          /* Current depth */
    
    /* Leaf traversal state */
    int leaf_index;                     /* Current index in leaf */
    const struct cls_rule *current;     /* Current rule */
};

struct dt_cursor dt_cursor_start(const struct decision_tree *dt,
                                 const struct cls_rule *target,
                                 ovs_version_t version);
void dt_cursor_advance(struct dt_cursor *cursor);

#define DT_FOR_EACH(RULE, MEMBER, DT) \
    DT_FOR_EACH_TARGET(RULE, MEMBER, DT, NULL, OVS_VERSION_MAX)

#define DT_FOR_EACH_TARGET(RULE, MEMBER, DT, TARGET, VERSION)           \
    for (struct dt_cursor cursor__ = dt_cursor_start(DT, TARGET, VERSION); \
         (cursor__.current                                              \
          ? (INIT_CONTAINER(RULE, cursor__.current, MEMBER),            \
             dt_cursor_advance(&cursor__),                              \
             true)                                                      \
          : false);                                                     \
        )

/* Defer/publish operations (batch optimization) */
void dt_defer(struct decision_tree *dt);
void dt_publish(struct decision_tree *dt);

/* Tree building */
struct dt_node *dt_build_tree(struct rculist *rules, size_t n_rules,
                              size_t max_leaf_size);

/* Lazy build: build tree from pending rules on first lookup or explicit call */
void dt_ensure_tree_built(struct decision_tree *dt);

/* Alias for dt_ensure_tree_built - more explicit name for initialization phase */
static inline void dt_build_initial_tree(struct decision_tree *dt) {
    dt_ensure_tree_built(dt);
}

/* Statistics */
void dt_get_stats(const struct decision_tree *dt,
                  size_t *n_rules, size_t *n_internal, size_t *n_leaf,
                  size_t *max_depth);

/* Helper functions */
static inline bool dt_is_empty(const struct decision_tree *dt) {
    return dt->n_rules == 0;
}

/* Print tree structure information (for debugging/testing) */
void dt_print_tree_info(const struct decision_tree *dt, const char *prefix);

#endif /* dt-classifier.h */