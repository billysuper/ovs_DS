#!/bin/bash
# Quick test script for DT bulk build
# Usage: ./run-dt-bulk-test.sh

set -e

echo "╔══════════════════════════════════════════════╗"
echo "║   DT Bulk Build Quick Test                  ║"
echo "╚══════════════════════════════════════════════╝"
echo ""

# Check if we're in the right directory
if [ ! -f "tests/test-dt-bulk.c" ]; then
    echo "Error: test-dt-bulk.c not found"
    echo "Please run this script from the OVS root directory"
    exit 1
fi

# Step 1: Check if dt-classifier files exist
echo "Step 1: Checking DT classifier files..."
if [ ! -f "lib/dt-classifier.c" ]; then
    echo "✗ lib/dt-classifier.c not found"
    exit 1
fi
if [ ! -f "lib/dt-classifier.h" ]; then
    echo "✗ lib/dt-classifier.h not found"
    exit 1
fi
echo "✓ DT classifier files found"
echo ""

# Step 2: Check if test file is in build system
echo "Step 2: Checking build system..."
if ! grep -q "test-dt-bulk.c" tests/automake.mk 2>/dev/null; then
    echo "⚠ Warning: test-dt-bulk.c not in tests/automake.mk"
    echo "   You may need to add it manually:"
    echo "   tests_ovstest_SOURCES += tests/test-dt-bulk.c"
    echo ""
fi

# Step 3: Compile
echo "Step 3: Compiling..."
echo "Running: make tests/ovstest"
if make tests/ovstest 2>&1 | tee /tmp/dt-build.log | grep -E "error:"; then
    echo "✗ Compilation failed"
    echo "See /tmp/dt-build.log for details"
    exit 1
fi

if [ ! -f "tests/ovstest" ]; then
    echo "✗ tests/ovstest not found after compilation"
    exit 1
fi

echo "✓ Compilation successful"
echo ""

# Step 4: Run test
echo "Step 4: Running tests..."
echo "Running: ./tests/ovstest test-dt-bulk"
echo ""
echo "════════════════════════════════════════════════"
./tests/ovstest test-dt-bulk
echo "════════════════════════════════════════════════"
echo ""

echo "✓ Test execution completed"
echo ""
echo "╔══════════════════════════════════════════════╗"
echo "║   Quick test finished successfully!          ║"
echo "╚══════════════════════════════════════════════╝"
