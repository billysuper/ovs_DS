#!/bin/bash
# Simple OVS Compilation Script

set -e

echo "==================================="
echo "  OVS Compilation Script"
echo "==================================="
echo

# Fix line endings if needed
echo "Step 1: Fixing line endings..."
if command -v dos2unix &> /dev/null; then
    find . -name "*.inc" -type f -exec dos2unix {} \; 2>/dev/null || true
    find . -name "*.man" -type f -exec dos2unix {} \; 2>/dev/null || true
    echo "  ✓ Line endings fixed"
else
    echo "  ⚠ dos2unix not found, skipping"
fi

# Remove problematic generated files
echo
echo "Step 2: Cleaning generated files..."
rm -f ovsdb/_server.ovsschema.inc
echo "  ✓ Cleaned"

# Compile
echo
echo "Step 3: Compiling OVS binaries..."
echo "  (This may take 2-3 minutes)"
echo

if make -j4 vswitchd/ovs-vswitchd utilities/ovs-vsctl ovsdb/ovsdb-server tests/ovstest 2>&1 | tee compile.log; then
    echo
    echo "✅ COMPILATION SUCCESSFUL!"
else
    echo
    echo "❌ Compilation failed. Last 30 lines:"
    tail -30 compile.log
    exit 1
fi

# Verify
echo
echo "Step 4: Verifying binaries..."
all_ok=true
for bin in vswitchd/ovs-vswitchd utilities/ovs-vsctl ovsdb/ovsdb-server tests/ovstest; do
    if [ -f "$bin" ]; then
        size=$(ls -lh "$bin" | awk '{print $5}')
        echo "  ✅ $bin ($size)"
    else
        echo "  ❌ $bin NOT FOUND"
        all_ok=false
    fi
done

if [ "$all_ok" = true ]; then
    echo
    echo "🎉 SUCCESS! OVS is ready to use."
    echo
    echo "Quick test:"
    echo "  ./tests/ovstest test-dt-lazy 10"
    echo
    echo "Run full test:"
    echo "  export OVS_CLASSIFIER_BACKEND=dt"
    echo "  ./tests/ovstest test-dt-lazy-realistic 100"
else
    echo
    echo "⚠️  Some binaries missing"
    exit 1
fi
