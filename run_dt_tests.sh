#!/bin/bash
# Build and run decision tree classifier tests

echo "=== Building Decision Tree Classifier Tests ==="

# Navigate to OVS directory
cd /mnt/d/ovs_DS || exit 1

# Build the test binary
echo "Building tests/ovstest..."
make tests/ovstest

if [ $? -ne 0 ]; then
    echo "ERROR: Build failed"
    exit 1
fi

echo ""
echo "=== Running Decision Tree Classifier Tests ==="
echo ""

# Run all tests
./tests/ovstest test-dt-classifier

if [ $? -eq 0 ]; then
    echo ""
    echo "=== All Tests Passed Successfully ==="
else
    echo ""
    echo "=== Tests Failed ==="
    exit 1
fi
