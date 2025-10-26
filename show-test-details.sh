#!/bin/bash
# 顯示測試的詳細內容

echo "=========================================="
echo "測試 #1: TSS 模式（默認）"
echo "=========================================="
echo ""
echo "環境設置："
unset OVS_CLASSIFIER_BACKEND
echo "  OVS_CLASSIFIER_BACKEND: (未設置，使用默認 TSS)"
echo ""
echo "執行測試..."
echo "----------------------------------------"
./tests/ovstest test-dt-lazy 2>&1
echo "----------------------------------------"
echo ""

echo "=========================================="
echo "測試 #2: DT 模式（決策樹）"
echo "=========================================="
echo ""
echo "環境設置："
export OVS_CLASSIFIER_BACKEND=dt
echo "  OVS_CLASSIFIER_BACKEND: dt"
echo ""
echo "執行測試..."
echo "----------------------------------------"
./tests/ovstest test-dt-lazy 2>&1
echo "----------------------------------------"
echo ""

echo "=========================================="
echo "測試 #3: 現實場景測試 (50-100 規則)"
echo "=========================================="
echo ""
echo "環境設置："
export OVS_CLASSIFIER_BACKEND=dt
echo "  OVS_CLASSIFIER_BACKEND: dt"
echo ""
echo "執行測試..."
echo "----------------------------------------"
./tests/ovstest test-dt-lazy-realistic 2>&1
echo "----------------------------------------"
echo ""

echo "=========================================="
echo "測試完成"
echo "=========================================="
