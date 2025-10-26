#!/bin/bash
# 簡單的測試腳本 - 快速驗證整合是否成功

set -e  # 遇到錯誤立即停止

# 顏色輸出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}OVS Classifier 快速測試${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# 檢查編譯
echo -e "${YELLOW}[步驟 1/4]${NC} 檢查編譯狀態..."
if [ ! -f "tests/ovstest" ]; then
    echo -e "${RED}❌ tests/ovstest 不存在，正在編譯...${NC}"
    make tests/ovstest
    if [ $? -ne 0 ]; then
        echo -e "${RED}❌ 編譯失敗！${NC}"
        exit 1
    fi
fi
echo -e "${GREEN}✅ 編譯檢查通過${NC}"
echo ""

# 測試 TSS 模式
echo -e "${YELLOW}[步驟 2/4]${NC} 測試 TSS 模式（默認）..."
unset OVS_CLASSIFIER_BACKEND
OUTPUT=$(./tests/ovstest test-dt-lazy 2>&1)
if echo "$OUTPUT" | grep -q "PASS"; then
    echo -e "${GREEN}✅ TSS 模式測試通過${NC}"
else
    echo -e "${RED}❌ TSS 模式測試失敗${NC}"
    echo "$OUTPUT" | tail -10
    exit 1
fi
echo ""

# 測試 DT 模式
echo -e "${YELLOW}[步驟 3/4]${NC} 測試 DT 模式..."
export OVS_CLASSIFIER_BACKEND=dt
OUTPUT=$(./tests/ovstest test-dt-lazy 2>&1)
if echo "$OUTPUT" | grep -q "PASS"; then
    echo -e "${GREEN}✅ DT 模式測試通過${NC}"
    # 顯示樹統計
    echo "$OUTPUT" | grep -E "internal nodes|leaf nodes|max depth" | head -3
else
    echo -e "${RED}❌ DT 模式測試失敗${NC}"
    echo "$OUTPUT" | tail -10
    exit 1
fi
echo ""

# 測試後端切換
echo -e "${YELLOW}[步驟 4/4]${NC} 測試後端切換..."
SWITCH_OK=true

# TSS -> DT
unset OVS_CLASSIFIER_BACKEND
./tests/ovstest test-dt-lazy >/dev/null 2>&1
export OVS_CLASSIFIER_BACKEND=dt
if ./tests/ovstest test-dt-lazy >/dev/null 2>&1; then
    echo -e "${GREEN}✅ TSS → DT 切換成功${NC}"
else
    echo -e "${RED}❌ TSS → DT 切換失敗${NC}"
    SWITCH_OK=false
fi

# DT -> TSS
export OVS_CLASSIFIER_BACKEND=tss
if ./tests/ovstest test-dt-lazy >/dev/null 2>&1; then
    echo -e "${GREEN}✅ DT → TSS 切換成功${NC}"
else
    echo -e "${RED}❌ DT → TSS 切換失敗${NC}"
    SWITCH_OK=false
fi

if [ "$SWITCH_OK" = false ]; then
    exit 1
fi
echo ""

# 總結
echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}🎉 所有測試通過！${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""
echo "測試結果："
echo "  ✅ TSS 模式正常"
echo "  ✅ DT 模式正常"
echo "  ✅ 後端切換正常"
echo ""
echo "使用方法："
echo "  默認 TSS:  ./vswitchd/ovs-vswitchd ..."
echo "  使用 DT:   export OVS_CLASSIFIER_BACKEND=dt"
echo "             ./vswitchd/ovs-vswitchd ..."
echo ""
echo "更多測試：./test-integration.sh"
echo "測試文檔：HOW_TO_TEST.md"
echo ""
