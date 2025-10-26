#!/bin/bash
# Check compilation progress

echo "=== Compilation Progress Check ==="
echo

# Check if make is running
if ps -p 6073 > /dev/null 2>&1; then
    echo "⏳ Compilation in progress (PID 6073 is running)"
    echo
    echo "Recent build output:"
    tail -15 build_final.log | grep -v "^$"
else
    echo "✅ Compilation process finished"
    echo
    
    # Check binaries
    echo "Checking binaries:"
    for f in vswitchd/ovs-vswitchd utilities/ovs-vsctl ovsdb/ovsdb-server tests/ovstest; do
        if [ -f "$f" ]; then
            size=$(ls -lh "$f" | awk '{print $5}')
            echo "  ✅ $f ($size)"
        else
            echo "  ❌ $f NOT FOUND"
        fi
    done
    
    echo
    echo "Last 20 lines of build log:"
    tail -20 build_final.log
fi
