#!/bin/bash
# Minimal DT bulk test runner

cd /mnt/d/ovs_DS

echo "=== Building tests/ovstest ==="
make tests/ovstest 2>&1 | grep -E "(error|Error|Success|successfully|test-dt-bulk)" | tail -20

if [ -f tests/ovstest ]; then
    echo ""
    echo "=== Running test-dt-bulk ==="
    ./tests/ovstest test-dt-bulk
else
    echo "ERROR: tests/ovstest not built"
    exit 1
fi
