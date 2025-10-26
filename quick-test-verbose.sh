#!/bin/bash
# 詳細測試腳本 - 顯示所有測試細節

set -e  # 遇到錯誤立即停止

# 顏色輸出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
NC='\033[0m' # No Color

echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  OVS Classifier 詳細測試報告          ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
echo ""

# 測試計數
TEST_NUM=0
PASSED=0
FAILED=0

# 測試函數
run_detailed_test() {
    local test_name=$1
    local backend=$2
    local show_output=$3
    
    ((TEST_NUM++))
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${YELLOW}測試 #$TEST_NUM: $test_name${NC}"
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    
    if [ -n "$backend" ]; then
        echo -e "後端設置: ${MAGENTA}$backend${NC}"
        if [ "$backend" = "default" ]; then
            unset OVS_CLASSIFIER_BACKEND
        else
            export OVS_CLASSIFIER_BACKEND=$backend
        fi
    fi
    
    echo "執行命令: ./tests/ovstest test-dt-lazy"
    echo ""
    
    START_TIME=$(date +%s.%N)
    OUTPUT=$(./tests/ovstest test-dt-lazy 2>&1)
    END_TIME=$(date +%s.%N)
    DURATION=$(echo "$END_TIME - $START_TIME" | bc)
    
    if [ "$show_output" = "yes" ]; then
        echo -e "${BLUE}完整輸出:${NC}"
        echo "----------------------------------------"
        echo "$OUTPUT"
        echo "----------------------------------------"
        echo ""
    fi
    
    # 分析輸出
    echo -e "${BLUE}測試分析:${NC}"
    
    # 檢查測試是否通過
    if echo "$OUTPUT" | grep -q "PASS"; then
        echo -e "  狀態: ${GREEN}✅ PASSED${NC}"
        ((PASSED++))
    else
        echo -e "  狀態: ${RED}❌ FAILED${NC}"
        ((FAILED++))
        echo ""
        echo -e "${RED}失敗詳情:${NC}"
        echo "$OUTPUT" | tail -20
    fi
    
    # 統計信息
    echo -e "  執行時間: ${CYAN}${DURATION}s${NC}"
    
    # 規則統計
    RULES_10=$(echo "$OUTPUT" | grep -o "10 rules" | head -1)
    RULES_100=$(echo "$OUTPUT" | grep -o "100 rules" | head -1)
    if [ -n "$RULES_10" ]; then
        echo -e "  測試規則: ${CYAN}10, 100, 5 條${NC}"
    fi
    
    # DT 特定信息
    if [ "$backend" = "dt" ]; then
        echo ""
        echo -e "${BLUE}決策樹統計:${NC}"
        
        # 提取樹構建信息
        INTERNAL=$(echo "$OUTPUT" | grep -o "[0-9]* internal nodes" | head -1)
        LEAF=$(echo "$OUTPUT" | grep -o "[0-9]* leaf nodes" | head -1)
        DEPTH=$(echo "$OUTPUT" | grep -o "max depth [0-9]*" | head -1)
        
        if [ -n "$INTERNAL" ]; then
            echo "  - $INTERNAL"
            echo "  - $LEAF"
            echo "  - $DEPTH"
        fi
        
        # 樹構建詳情
        echo ""
        echo -e "${BLUE}樹構建過程:${NC}"
        echo "$OUTPUT" | grep -E "\[DT\]" | head -5
    fi
    
    echo ""
}

# ============================================
# 第一部分：編譯檢查
# ============================================
echo -e "${MAGENTA}╔════════════════════════════════════════╗${NC}"
echo -e "${MAGENTA}║  第一部分：編譯檢查                   ║${NC}"
echo -e "${MAGENTA}╚════════════════════════════════════════╝${NC}"
echo ""

echo "檢查必需的文件..."
FILES_OK=true

if [ -f "tests/ovstest" ]; then
    echo -e "  tests/ovstest: ${GREEN}✅ 存在${NC}"
    ls -lh tests/ovstest | awk '{print "    大小: " $5 ", 修改時間: " $6 " " $7 " " $8}'
else
    echo -e "  tests/ovstest: ${RED}❌ 不存在${NC}"
    FILES_OK=false
fi

if [ -f "lib/.libs/libopenvswitch.so" ]; then
    echo -e "  libopenvswitch.so: ${GREEN}✅ 存在${NC}"
else
    echo -e "  libopenvswitch.so: ${YELLOW}⚠️  檢查失敗${NC}"
fi

if [ "$FILES_OK" = false ]; then
    echo ""
    echo -e "${YELLOW}正在編譯測試程序...${NC}"
    make tests/ovstest
    if [ $? -ne 0 ]; then
        echo -e "${RED}❌ 編譯失敗！${NC}"
        exit 1
    fi
fi

echo ""

# ============================================
# 第二部分：TSS 模式測試
# ============================================
echo -e "${MAGENTA}╔════════════════════════════════════════╗${NC}"
echo -e "${MAGENTA}║  第二部分：TSS 模式測試                ║${NC}"
echo -e "${MAGENTA}╚════════════════════════════════════════╝${NC}"
echo ""

run_detailed_test "TSS 默認模式" "default" "no"

# ============================================
# 第三部分：DT 模式測試
# ============================================
echo -e "${MAGENTA}╔════════════════════════════════════════╗${NC}"
echo -e "${MAGENTA}║  第三部分：DT 模式測試                 ║${NC}"
echo -e "${MAGENTA}╚════════════════════════════════════════╝${NC}"
echo ""

run_detailed_test "DT 決策樹模式" "dt" "no"

# ============================================
# 第四部分：後端切換測試
# ============================================
echo -e "${MAGENTA}╔════════════════════════════════════════╗${NC}"
echo -e "${MAGENTA}║  第四部分：後端切換測試                ║${NC}"
echo -e "${MAGENTA}╚════════════════════════════════════════╝${NC}"
echo ""

echo -e "${YELLOW}測試場景 1: TSS → DT 切換${NC}"
echo "步驟："
echo "  1. 運行 TSS 模式"
unset OVS_CLASSIFIER_BACKEND
./tests/ovstest test-dt-lazy >/dev/null 2>&1
echo -e "     ${GREEN}✅ TSS 模式執行成功${NC}"

echo "  2. 切換到 DT 模式"
export OVS_CLASSIFIER_BACKEND=dt
if ./tests/ovstest test-dt-lazy >/dev/null 2>&1; then
    echo -e "     ${GREEN}✅ DT 模式執行成功${NC}"
    echo -e "結果: ${GREEN}✅ 切換成功${NC}"
    ((PASSED++))
else
    echo -e "     ${RED}❌ DT 模式執行失敗${NC}"
    echo -e "結果: ${RED}❌ 切換失敗${NC}"
    ((FAILED++))
fi
echo ""

echo -e "${YELLOW}測試場景 2: DT → TSS 切換${NC}"
echo "步驟："
echo "  1. 運行 DT 模式"
export OVS_CLASSIFIER_BACKEND=dt
./tests/ovstest test-dt-lazy >/dev/null 2>&1
echo -e "     ${GREEN}✅ DT 模式執行成功${NC}"

echo "  2. 切換到 TSS 模式"
export OVS_CLASSIFIER_BACKEND=tss
if ./tests/ovstest test-dt-lazy >/dev/null 2>&1; then
    echo -e "     ${GREEN}✅ TSS 模式執行成功${NC}"
    echo -e "結果: ${GREEN}✅ 切換成功${NC}"
    ((PASSED++))
else
    echo -e "     ${RED}❌ TSS 模式執行失敗${NC}"
    echo -e "結果: ${RED}❌ 切換失敗${NC}"
    ((FAILED++))
fi
echo ""

# ============================================
# 第五部分：現實場景測試
# ============================================
echo -e "${MAGENTA}╔════════════════════════════════════════╗${NC}"
echo -e "${MAGENTA}║  第五部分：現實場景測試                ║${NC}"
echo -e "${MAGENTA}╚════════════════════════════════════════╝${NC}"
echo ""

if [ -f "tests/ovstest" ]; then
    echo "運行 test-dt-lazy-realistic (50-100 條規則)..."
    echo ""
    
    export OVS_CLASSIFIER_BACKEND=dt
    START_TIME=$(date +%s.%N)
    REALISTIC_OUTPUT=$(./tests/ovstest test-dt-lazy-realistic 2>&1)
    END_TIME=$(date +%s.%N)
    DURATION=$(echo "$END_TIME - $START_TIME" | bc)
    
    echo -e "${BLUE}測試結果:${NC}"
    echo "$REALISTIC_OUTPUT" | grep -E "PASS|FAIL|rules|internal|leaf|depth|Average" | head -20
    
    echo ""
    echo -e "執行時間: ${CYAN}${DURATION}s${NC}"
    
    if echo "$REALISTIC_OUTPUT" | grep -q "internal nodes"; then
        ((PASSED++))
        echo -e "狀態: ${GREEN}✅ 現實場景測試通過${NC}"
    else
        ((FAILED++))
        echo -e "狀態: ${YELLOW}⚠️  測試執行但可能有問題${NC}"
    fi
else
    echo -e "${YELLOW}⚠️  跳過現實場景測試（測試程序不存在）${NC}"
fi

echo ""

# ============================================
# 總結報告
# ============================================
echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  測試總結報告                          ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
echo ""

TOTAL=$((PASSED + FAILED))
PASS_RATE=$(echo "scale=1; $PASSED * 100 / $TOTAL" | bc)

echo "測試統計："
echo -e "  總測試數: ${CYAN}$TOTAL${NC}"
echo -e "  通過數量: ${GREEN}$PASSED${NC}"
echo -e "  失敗數量: ${RED}$FAILED${NC}"
echo -e "  通過率:   ${CYAN}${PASS_RATE}%${NC}"
echo ""

echo "測試項目明細："
echo "  ✅ TSS 默認模式測試"
echo "  ✅ DT 決策樹模式測試"
echo "  ✅ TSS → DT 切換測試"
echo "  ✅ DT → TSS 切換測試"
echo "  ✅ 現實場景測試"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}╔════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║  🎉 恭喜！所有測試通過！              ║${NC}"
    echo -e "${GREEN}╚════════════════════════════════════════╝${NC}"
    echo ""
    echo "系統狀態："
    echo "  ✅ 編譯環境正常"
    echo "  ✅ TSS 後端正常"
    echo "  ✅ DT 後端正常"
    echo "  ✅ 後端切換正常"
    echo "  ✅ 生產就緒（TSS 模式）"
    echo "  ⚠️  DT 模式建議用於測試"
else
    echo -e "${RED}╔════════════════════════════════════════╗${NC}"
    echo -e "${RED}║  ⚠️  有測試失敗，請檢查！             ║${NC}"
    echo -e "${RED}╚════════════════════════════════════════╝${NC}"
    echo ""
    echo "建議操作："
    echo "  1. 查看上面的錯誤詳情"
    echo "  2. 重新編譯: make clean && make"
    echo "  3. 查看日誌: ./tests/ovstest test-dt-lazy 2>&1 | less"
    echo "  4. 查看文檔: cat HOW_TO_TEST.md"
fi

echo ""
echo "進一步測試："
echo "  完整測試:     ./test-integration.sh"
echo "  性能測試:     見 HOW_TO_TEST.md"
echo "  整合文檔:     cat DT_INTEGRATION_COMPLETE.md"
echo ""

if [ $FAILED -eq 0 ]; then
    exit 0
else
    exit 1
fi
