#!/bin/bash
# Decision Tree Classifier - 測試腳本

echo "========================================="
echo "DT Classifier 編譯和測試腳本"
echo "========================================="
echo

# 切換到項目目錄
cd /mnt/d/ovs_DS || exit 1

echo "步驟 1: 重新編譯 dt-classifier.lo"
echo "-----------------------------------------"
make lib/dt-classifier.lo 2>&1 | tail -n 10
if [ $? -ne 0 ]; then
    echo "❌ 編譯失敗！"
    exit 1
fi
echo "✅ 編譯成功"
echo

echo "步驟 2: 重新鏈接 dt-test"
echo "-----------------------------------------"
./libtool --mode=link gcc -o dt-test \
    lib/dt-classifier-test.c \
    lib/dt-classifier.lo \
    lib/.libs/libopenvswitch.a \
    -I. -Iinclude \
    -lpthread -lrt -lm -lssl -lcrypto -lcap-ng 2>&1 | tail -n 5

if [ $? -ne 0 ]; then
    echo "❌ 鏈接失敗！"
    exit 1
fi
echo "✅ 鏈接成功"
echo

echo "步驟 3: 運行基礎測試"
echo "-----------------------------------------"
./dt-test-minimal
echo

echo "步驟 4: 運行完整測試"
echo "-----------------------------------------"
./dt-test
echo

echo "========================================="
echo "測試完成！"
echo "========================================="
