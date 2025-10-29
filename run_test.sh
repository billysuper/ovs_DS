#!/bin/bash
cd /mnt/d/ovs_DS
echo "Building tests/ovstest..."
make tests/ovstest 2>&1 | tail -20
if [ $? -eq 0 ]; then
    echo ""
    echo "Running test-dt-classifier..."
    ./tests/ovstest test-dt-classifier
else
    echo "Build failed!"
    exit 1
fi
