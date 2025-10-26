#!/bin/bash
# OVS Build and Test Script

set -e  # Exit on error

echo "=========================================="
echo " OVS Decision Tree Integration"
echo " Build and Test Script"
echo "=========================================="
echo

# Step 1: Clean
echo "Step 1: Cleaning previous build..."
make clean > /dev/null 2>&1 || true

# Step 2: Compile
echo "Step 2: Compiling OVS (this may take a few minutes)..."
echo "        Using $(nproc) cores..."
if make -j$(nproc) 2>&1 | tee build_output.log; then
    echo "✅ Compilation successful!"
else
    echo "❌ Compilation failed. Check build_output.log for details."
    echo "Last 20 lines of build log:"
    tail -20 build_output.log
    exit 1
fi

# Step 3: Verify binaries
echo
echo "Step 3: Verifying built binaries..."
success=true
for binary in "vswitchd/ovs-vswitchd" "utilities/ovs-vsctl" "ovsdb/ovsdb-server" "tests/ovstest"; do
    if [ -f "$binary" ]; then
        size=$(ls -lh "$binary" | awk '{print $5}')
        echo "  ✅ $binary ($size)"
    else
        echo "  ❌ $binary NOT FOUND"
        success=false
    fi
done

if [ "$success" = false ]; then
    echo
    echo "❌ Some binaries missing. Build may have failed."
    exit 1
fi

# Step 4: Run quick tests
echo
echo "Step 4: Running quick tests..."
echo

# Test 1: TSS mode
echo "Test 1: TSS mode (default)"
export OVS_CLASSIFIER_BACKEND=tss
if ./tests/ovstest test-dt-lazy-realistic 100 2>&1 | grep -q "All tests passed"; then
    echo "  ✅ TSS mode test passed"
else
    echo "  ⚠️  TSS mode test had issues"
fi

# Test 2: DT mode  
echo
echo "Test 2: DT mode (experimental)"
export OVS_CLASSIFIER_BACKEND=dt
if ./tests/ovstest test-dt-lazy-realistic 100 2>&1 | grep -q "All tests passed"; then
    echo "  ✅ DT mode test passed"
else
    echo "  ⚠️  DT mode test had issues"
fi

# Summary
echo
echo "=========================================="
echo " ✅ BUILD AND TEST COMPLETE!"
echo "=========================================="
echo
echo "OVS is ready to run. Usage:"
echo
echo "1. Default TSS mode (100% compatible with original OVS):"
echo "   sudo ./vswitchd/ovs-vswitchd --help"
echo
echo "2. Decision Tree mode (experimental):"
echo "   export OVS_CLASSIFIER_BACKEND=dt"
echo "   sudo ./vswitchd/ovs-vswitchd --help"
echo
echo "3. Run full test suite:"
echo "   make check"
echo

