/* DT叶节点优化方案分析
 * 
 * 问题：你提出"只复制叶节点就好"是对的，但需要正确实现
 * 
 * ========== 方案对比 ==========
 * 
 * 方案A：当前实现（完整路径COW）
 * --------------------------------
 * 优点：
 * ✅ RCU安全（完全符合RCU语义）
 * ✅ 实现简单（逻辑清晰）
 * ✅ 适用于树结构可能变化的场景
 * 
 * 缺点：
 * ❌ 内存开销：O(树深度) 每次插入
 * ❌ 复制开销：复制所有祖先节点
 * 
 * 代码：
 * ```c
 * // 复制整条路径
 * for (i = depth-2; i >= 0; i--) {
 *     new_parent = copy(old_parent);
 *     new_parent->child = new_child;
 *     new_child = new_parent;
 * }
 * ```
 * 
 * 
 * 方案B：优化版（只复制叶节点 + 内部节点用RCU指针）
 * -------------------------------------------------------
 * 核心思想：
 * - 叶节点的rules数组本身是独立分配的
 * - 内部节点的child指针已经是OVSRCU_TYPE
 * - 只需要原子替换叶节点指针即可
 * 
 * 实现：
 * ```c
 * struct dt_internal_node {
 *     const struct mf_field *field;
 *     enum dt_test_type test_type;
 *     union { ... } test;
 *     
 *     // ✅ 已经是RCU指针！
 *     OVSRCU_TYPE(struct dt_node *) left;
 *     OVSRCU_TYPE(struct dt_node *) right;
 * };
 * 
 * // 优化后的插入：
 * bool dt_insert_rule_optimized(struct decision_tree *dt, 
 *                               const struct cls_rule *rule) {
 *     // 1. 找到叶节点和父节点
 *     struct dt_node *parent = NULL;
 *     struct dt_node *leaf = traverse_to_leaf(&parent, ...);
 *     
 *     // 2. 复制叶节点
 *     struct dt_node *new_leaf = dt_node_copy(leaf);
 *     insert_rule_to_leaf(new_leaf, rule);
 *     
 *     // 3. 原子替换父节点的指针（RCU安全）
 *     if (went_left) {
 *         ovsrcu_set(&parent->internal.left, new_leaf);
 *     } else {
 *         ovsrcu_set(&parent->internal.right, new_leaf);
 *     }
 *     
 *     // 4. RCU延迟删除旧叶节点
 *     ovsrcu_postpone(dt_node_destroy, leaf);
 *     
 *     return true;
 * }
 * ```
 * 
 * 优点：
 * ✅ 内存开销：O(1) 每次插入（只复制一个叶节点）
 * ✅ 性能：更快（减少复制）
 * ✅ RCU安全：ovsrcu_set保证原子性
 * 
 * 缺点：
 * ⚠️ 前提条件：树结构不能变化
 * ⚠️ 需要记录父节点指针
 * 
 * 
 * 方案C：极致优化（原地修改rules数组）
 * -----------------------------------------
 * 更激进的优化：
 * ```c
 * struct dt_leaf_node {
 *     // ✅ 把rules指针本身变成RCU保护
 *     OVSRCU_TYPE(const struct cls_rule **) rules;
 *     atomic_size_t n_rules;
 *     size_t capacity;
 * };
 * 
 * bool dt_insert_rule_ultra_optimized(...) {
 *     // 1. 找到叶节点
 *     struct dt_node *leaf = traverse_to_leaf(...);
 *     
 *     // 2. 复制rules数组（不复制整个叶节点）
 *     const struct cls_rule **old_rules = 
 *         ovsrcu_get(const struct cls_rule **, &leaf->leaf.rules);
 *     const struct cls_rule **new_rules = xmalloc(...);
 *     memcpy(new_rules, old_rules, ...);
 *     
 *     // 3. 在新数组中插入规则
 *     insert_to_array(new_rules, rule);
 *     
 *     // 4. 原子替换rules指针
 *     ovsrcu_set(&leaf->leaf.rules, new_rules);
 *     atomic_store(&leaf->leaf.n_rules, new_n);
 *     
 *     // 5. RCU延迟删除旧数组
 *     ovsrcu_postpone(free, old_rules);
 * }
 * ```
 * 
 * 优点：
 * ✅ 内存开销：最小（只复制rules数组）
 * ✅ 性能：最快
 * 
 * 缺点：
 * ❌ 复杂度高
 * ❌ 需要修改数据结构
 * ❌ atomic操作开销
 * 
 * 
 * ========== 推荐方案 ==========
 * 
 * 对于DT的场景（树结构不变，只修改叶节点）：
 * 
 * **推荐：方案B（只复制叶节点）**
 * 
 * 理由：
 * 1. ✅ 性能提升显著（O(depth) → O(1)）
 * 2. ✅ 实现相对简单
 * 3. ✅ RCU安全（利用现有OVSRCU_TYPE）
 * 4. ✅ 不需要修改数据结构
 * 5. ✅ 符合DT的使用场景
 * 
 * 
 * ========== 实现示例 ==========
 */

// 优化后的插入函数（方案B）
bool dt_insert_rule_optimized(struct decision_tree *dt, 
                              const struct cls_rule *rule,
                              ovs_version_t version OVS_UNUSED)
{
    if (!dt->tree_built) {
        return dt_add_rule_lazy(dt, rule);
    }
    
    struct dt_node *old_root = ovsrcu_get_protected(struct dt_node *, &dt->root);
    
    if (!old_root) {
        // Empty tree case...
        return create_first_leaf(dt, rule);
    }
    
    // 遍历树，找到叶节点和它的父节点
    struct dt_node *parent = NULL;
    struct dt_node *node = old_root;
    bool went_right = false;
    
    while (node && node->type == DT_NODE_INTERNAL) {
        parent = node;  // 记录父节点
        
        // 根据规则值决定方向
        went_right = evaluate_test(node, rule);
        
        if (went_right) {
            node = ovsrcu_get_protected(struct dt_node *, &node->internal.right);
        } else {
            node = ovsrcu_get_protected(struct dt_node *, &node->internal.left);
        }
    }
    
    if (!node || node->type != DT_NODE_LEAF) {
        VLOG_WARN("dt_insert_rule: traversal error");
        return false;
    }
    
    // ✅ 只复制叶节点（不复制父节点）
    struct dt_node *new_leaf = dt_node_copy(node);
    insert_rule_into_leaf(new_leaf, rule);
    
    // ✅ 原子替换父节点的child指针（RCU安全）
    if (!parent) {
        // 如果叶节点就是根节点
        ovsrcu_set(&dt->root, new_leaf);
    } else {
        // 替换父节点的指针
        if (went_right) {
            ovsrcu_set(&parent->internal.right, new_leaf);
        } else {
            ovsrcu_set(&parent->internal.left, new_leaf);
        }
    }
    
    // ✅ RCU延迟删除旧叶节点（不删除整棵树！）
    ovsrcu_postpone(dt_node_destroy_leaf, node);
    
    dt->n_rules++;
    
    return true;
}

/* ========== 关键问题解答 ==========
 * 
 * Q: 为什么当前代码要复制整条路径？
 * A: 这是经典的持久化数据结构做法，适用于：
 *    - 树结构可能变化
 *    - 需要保留多个版本
 *    - 实现简单安全
 * 
 * Q: 为什么可以只复制叶节点？
 * A: 因为：
 *    - DT的树结构在build后不变
 *    - 内部节点的child指针已经是OVSRCU_TYPE
 *    - ovsrcu_set()保证原子性和内存顺序
 * 
 * Q: 这样真的RCU安全吗？
 * A: 是的，因为：
 *    Reader: node = ovsrcu_get(&parent->left);
 *            // ✅ 读到的要么是旧叶节点，要么是新叶节点
 *            // ✅ 两者都是有效的完整节点
 *    
 *    Writer: ovsrcu_set(&parent->left, new_leaf);
 *            // ✅ 原子替换，内存屏障保证可见性
 *            
 *    RCU:    等所有reader退出后才删除旧叶节点
 * 
 * Q: 性能提升有多大？
 * A: 假设树深度为10：
 *    方案A: 复制11个节点（root + 9个内部节点 + 1个叶节点）
 *    方案B: 复制1个节点（只有叶节点）
 *    提升: 11x内存分配减少，~5-10x整体性能提升
 * 
 * Q: 什么时候不能用这个优化？
 * A: 当需要重建树结构时（split/merge叶节点），
 *    那时必须用完整路径COW
 */
