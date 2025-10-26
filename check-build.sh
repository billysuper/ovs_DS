#!/bin/bash
# Check OVS build status

echo "=== Checking OVS Build Status ==="
echo

if [ -f "vswitchd/ovs-vswitchd" ]; then
    echo "✅ ovs-vswitchd: $(ls -lh vswitchd/ovs-vswitchd | awk '{print $5, $6, $7, $8}')"
else
    echo "❌ ovs-vswitchd: NOT FOUND"
fi

if [ -f "utilities/ovs-vsctl" ]; then
    echo "✅ ovs-vsctl: $(ls -lh utilities/ovs-vsctl | awk '{print $5, $6, $7, $8}')"
else
    echo "❌ ovs-vsctl: NOT FOUND"
fi

if [ -f "tests/ovstest" ]; then
    echo "✅ ovstest: $(ls -lh tests/ovstest | awk '{print $5, $6, $7, $8}')"
else
    echo "❌ ovstest: NOT FOUND"
fi

echo
if [ -f "vswitchd/ovs-vswitchd" ] && [ -f "utilities/ovs-vsctl" ]; then
    echo "🎉 BUILD SUCCESSFUL! OVS is ready to run."
    echo
    echo "To run OVS:"
    echo "  # Default TSS mode (safe):"
    echo "  sudo ./vswitchd/ovs-vswitchd --pidfile --detach"
    echo
    echo "  # Decision Tree mode (experimental):"
    echo "  export OVS_CLASSIFIER_BACKEND=dt"
    echo "  sudo ./vswitchd/ovs-vswitchd --pidfile --detach"
else
    echo "⚠️  Build incomplete. Run: make -j4"
fi
