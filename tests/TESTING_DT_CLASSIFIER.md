# Decision Tree Classifier Testing

This test suite verifies the correctness of the decision tree classifier implementation using a dual-classifier approach similar to OVS's native classifier tests.

## Test Strategy

The test suite uses **dual-classifier verification**:
- **Decision Tree Classifier**: The implementation being tested
- **Simple Linear Classifier**: Reference implementation (just a linear list of rules)

Both classifiers receive the same rules, and their lookup results are compared across thousands of test flows to ensure correctness.

## Test Files

- `test-dt-classifier.c` - Main test implementation
- `dt-classifier.at` - Autotest definitions for OVS test suite
- `tests/automake.mk` - Updated to include decision tree tests

## Test Cases

### 1. **test_dt_empty**
Tests lookup in an empty decision tree (should return NULL).

### 2. **test_dt_single_rule**
Tests tree with a single rule:
- Matching flow should return the rule
- Non-matching flow should return NULL

### 3. **test_dt_priority_ordering**
Tests priority-based matching with overlapping rules:
- Catch-all rule (priority 10)
- IP-specific rule (priority 100)
- IP+port specific rule (priority 1000)

Verifies that highest priority matching rule is always returned.

### 4. **test_dt_dual_classifier**
Core verification test with 50 random rules:
- Same rules added to both decision tree and simple classifier
- Performs lookups on all possible flow combinations
- Compares results to ensure identical behavior

### 5. **test_dt_many_rules**
Stress test with 200 random rules to verify:
- Correct tree construction with many rules
- Proper handling of diverse rule patterns
- Correctness under load

### 6. **test_dt_benchmark**
Performance benchmark with 500 rules:
- Measures tree build time
- Performs 100,000 lookups
- Reports throughput (lookups/ms)
- Reports average latency (μs/lookup)

## How to Run Tests

### Run All Tests
```bash
ovstest test-dt-classifier
```

### Run Specific Test
```bash
ovstest test-dt-classifier empty
ovstest test-dt-classifier single-rule
ovstest test-dt-classifier priority
ovstest test-dt-classifier dual
ovstest test-dt-classifier many
ovstest test-dt-classifier benchmark
```

### Run Through Autotest Framework
```bash
cd tests
make check TESTSUITEFLAGS='-k dt-classifier'
```

Or run specific autotest:
```bash
make check TESTSUITEFLAGS='-k "decision tree - dual"'
```

## Test Flow Generation

Test flows are generated from predefined values to ensure comprehensive coverage:

### Predefined Values
- **nw_src**: 0, 10.0.0.1, 10.0.0.2, 192.168.1.1, 192.168.1.2 (5 values)
- **nw_dst**: 0, 10.0.0.1, 10.0.0.2, 192.168.1.1, 192.168.1.2 (5 values)
- **tp_src**: 0, 80, 443, 8080 (4 values)
- **tp_dst**: 0, 80, 443, 8080 (4 values)
- **nw_proto**: 0, TCP, UDP, ICMP (4 values)
- **in_port**: 0, 1, 2, 3 (4 values)

### Total Combinations
N_FLOW_VALUES = 5 × 5 × 4 × 4 × 4 × 4 = **1,600 unique flows**

Each test verifies all possible combinations to ensure comprehensive coverage.

## Expected Output

### Successful Test Run
```
=== Running Decision Tree Classifier Tests ===

PASSED: empty tree test
PASSED: single rule test
PASSED: priority ordering test
Building dual classifiers with random rules...
Decision tree stats:
  Tree built: YES
  Total rules: 50
  Internal nodes: 15
  Leaf nodes: 16
  Max depth: 8
Comparing 1600 lookups................
PASSED: All 1600 lookups matched!
...

=== All Tests Passed ===
```

### Failed Test Example
```
Flow 123: DT priority=1000, Simple priority=500
FAILED: 1 errors out of 1600 lookups
```

## Integration with OVS Build System

The test is registered with OVSTEST_REGISTER() and integrated into:
1. `tests/automake.mk` - Added test-dt-classifier.c to build
2. `tests/dt-classifier.at` - Autotest definitions
3. OVS test suite - Runs with `make check`

## Debugging Tips

### Enable Debug Logging
```bash
ovs-appctl vlog/set dt_classifier:dbg
```

### Check Tree Structure
The test prints tree statistics including:
- Number of rules
- Number of internal/leaf nodes
- Maximum tree depth
- Average rules per leaf
- Tree structure (for small trees)

### Compare with Simple Classifier
The dual-classifier approach makes debugging easy:
- Simple classifier is trivially correct (linear search)
- Any difference indicates a bug in decision tree
- Error messages show exactly which flow failed

## Performance Expectations

Benchmark test with 500 rules typically shows:
- **Build time**: 10-50 ms (depends on rule complexity)
- **Throughput**: 10,000-50,000 lookups/ms
- **Latency**: 20-100 μs/lookup

Performance varies based on:
- Tree depth
- Rule distribution
- Field selection quality
- CPU speed

## Known Limitations

1. **Wildcard tracking**: Currently TODO, not fully implemented
2. **RCU version visibility**: Basic implementation for standalone testing
3. **Field selection**: Uses simple heuristic, not information gain
4. **Test coverage**: Focuses on IPv4 flows, limited IPv6/MPLS testing

## Future Enhancements

1. Add permutation testing (random insertion order)
2. Add rule deletion tests
3. Add concurrent access tests (RCU verification)
4. Add memory leak detection
5. Add IPv6 and MPLS test flows
6. Add prefix/range matching tests
