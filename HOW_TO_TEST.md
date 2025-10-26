# OVS 決策樹整合測試指南

本文檔說明如何測試 OVS Classifier 的 TSS 和 DT 雙後端實現。

---

## 測試準備

### 1. 編譯系統

```bash
cd /mnt/d/ovs_DS

# 完整編譯（推薦）
make clean
make -j4

# 或只編譯需要的部分
make lib/libopenvswitch.la
make tests/ovstest
```

### 2. 檢查編譯結果

```bash
# 檢查庫文件
ls -lh lib/.libs/libopenvswitch.so*

# 檢查測試程序
ls -lh tests/ovstest
```

---

## 測試方法

### 方法一：快速測試（推薦入門）

運行自動化整合測試：

```bash
cd /mnt/d/ovs_DS
./test-integration.sh
```

**預期輸出：**
```
================================
Testing Classifier Integration
================================

[1/5] Building test programs...
✅ Build successful

[2/5] Testing TSS mode (default)...
✅ TSS mode works

[3/5] Testing explicit TSS mode...
✅ Explicit TSS mode works

[4/5] Testing DT mode...
✅ DT mode executed

[5/5] Running DT-specific test...
✅ ALL TESTS PASSED!
```

---

### 方法二：手動測試各個模式

#### A. 測試 TSS 模式（默認）

```bash
# 1. 不設置環境變量（默認 TSS）
unset OVS_CLASSIFIER_BACKEND

# 2. 運行 DT 測試（應該使用 TSS）
./tests/ovstest test-dt-lazy

# 3. 檢查輸出
# 應該看到 "Using TSS classifier backend"
```

#### B. 測試 DT 模式

```bash
# 1. 設置環境變量
export OVS_CLASSIFIER_BACKEND=dt

# 2. 運行 DT 測試
./tests/ovstest test-dt-lazy

# 3. 檢查輸出
# 應該看到決策樹構建信息：
# "DT Lazy Build: Tree built successfully - X rules, Y internal nodes, Z leaf nodes"
```

#### C. 測試現實場景

```bash
# 1. 設置 DT 模式
export OVS_CLASSIFIER_BACKEND=dt

# 2. 運行現實場景測試
./tests/ovstest test-dt-lazy-realistic

# 3. 查看樹結構統計
# 應該看到：
# - 總規則數
# - 內部節點數
# - 葉節點數  
# - 最大深度
# - 平均每葉規則數
```

---

### 方法三：測試真實 OVS 流表

#### 準備工作

```bash
# 1. 編譯 ovs-vswitchd
make vswitchd/ovs-vswitchd

# 2. 設置後端
export OVS_CLASSIFIER_BACKEND=dt  # 或 tss

# 3. 查看日誌確認後端
# 啟動 ovs-vswitchd 時應該看到：
# "Using Decision Tree classifier backend" 或
# "Using TSS classifier backend"
```

---

## 測試腳本

### 完整測試腳本

創建 `run-all-tests.sh`：

```bash
#!/bin/bash

echo "=========================================="
echo "OVS Classifier 完整測試套件"
echo "=========================================="
echo ""

# 測試計數器
PASSED=0
FAILED=0

# 測試函數
run_test() {
    local name=$1
    local cmd=$2
    echo "測試: $name"
    echo "命令: $cmd"
    eval $cmd
    if [ $? -eq 0 ]; then
        echo "✅ PASSED"
        ((PASSED++))
    else
        echo "❌ FAILED"
        ((FAILED++))
    fi
    echo ""
}

# 1. TSS 默認模式測試
echo "=== 第一組：TSS 默認模式 ==="
unset OVS_CLASSIFIER_BACKEND
run_test "TSS-1: 基本功能" "./tests/ovstest test-dt-lazy 2>&1 | grep -q 'PASS'"

# 2. TSS 顯式模式測試
echo "=== 第二組：TSS 顯式模式 ==="
export OVS_CLASSIFIER_BACKEND=tss
run_test "TSS-2: 顯式指定" "./tests/ovstest test-dt-lazy 2>&1 | grep -q 'PASS'"

# 3. DT 模式測試
echo "=== 第三組：DT 決策樹模式 ==="
export OVS_CLASSIFIER_BACKEND=dt
run_test "DT-1: 基本測試" "./tests/ovstest test-dt-lazy 2>&1 | grep -q 'PASS'"
run_test "DT-2: 現實場景" "./tests/ovstest test-dt-lazy-realistic 2>&1 | grep -q 'internal nodes'"

# 4. 後端切換測試
echo "=== 第四組：後端切換測試 ==="
unset OVS_CLASSIFIER_BACKEND
run_test "切換-1: TSS->DT" "export OVS_CLASSIFIER_BACKEND=dt && ./tests/ovstest test-dt-lazy 2>&1 | grep -q 'PASS'"
run_test "切換-2: DT->TSS" "export OVS_CLASSIFIER_BACKEND=tss && ./tests/ovstest test-dt-lazy 2>&1 | grep -q 'PASS'"

# 總結
echo "=========================================="
echo "測試總結"
echo "=========================================="
echo "通過: $PASSED"
echo "失敗: $FAILED"
echo "總計: $((PASSED + FAILED))"
echo ""

if [ $FAILED -eq 0 ]; then
    echo "🎉 所有測試通過！"
    exit 0
else
    echo "⚠️  有 $FAILED 個測試失敗"
    exit 1
fi
```

使用方法：
```bash
chmod +x run-all-tests.sh
./run-all-tests.sh
```

---

## 性能測試

### 1. 查找性能測試

創建 `benchmark-lookup.sh`：

```bash
#!/bin/bash

echo "Classifier 查找性能基準測試"
echo "=============================="
echo ""

# 測試不同規則數量
for N_RULES in 10 50 100 500 1000; do
    echo "測試 $N_RULES 條規則..."
    
    # TSS 模式
    export OVS_CLASSIFIER_BACKEND=tss
    echo -n "  TSS: "
    time ./tests/ovstest test-dt-lazy 2>&1 | grep -o "lookup.*ms" | head -1
    
    # DT 模式
    export OVS_CLASSIFIER_BACKEND=dt
    echo -n "  DT:  "
    time ./tests/ovstest test-dt-lazy 2>&1 | grep -o "lookup.*ms" | head -1
    
    echo ""
done
```

### 2. 內存使用測試

```bash
#!/bin/bash

echo "內存使用測試"
echo "============"

# TSS 模式
export OVS_CLASSIFIER_BACKEND=tss
echo "TSS 模式："
/usr/bin/time -v ./tests/ovstest test-dt-lazy-realistic 2>&1 | grep "Maximum resident"

# DT 模式
export OVS_CLASSIFIER_BACKEND=dt
echo "DT 模式："
/usr/bin/time -v ./tests/ovstest test-dt-lazy-realistic 2>&1 | grep "Maximum resident"
```

---

## 調試技巧

### 1. 啟用詳細日誌

```bash
# 查看所有 DT 相關日誌
export OVS_CLASSIFIER_BACKEND=dt
./tests/ovstest test-dt-lazy 2>&1 | grep -E "\[DT\]|DT_|INFO"
```

### 2. 查看樹結構

```bash
# 運行測試並查看樹的詳細統計
export OVS_CLASSIFIER_BACKEND=dt
./tests/ovstest test-dt-lazy-realistic 2>&1 | grep -A 10 "Decision Tree Structure"
```

### 3. 檢查後端選擇

```bash
# 創建測試程序驗證後端
cat > test-backend.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>

int main() {
    const char *backend = getenv("OVS_CLASSIFIER_BACKEND");
    printf("配置的後端: %s\n", backend ? backend : "tss (default)");
    return 0;
}
EOF

gcc test-backend.c -o test-backend
./test-backend
```

---

## 常見問題排查

### Q1: 測試程序找不到

```bash
# 檢查編譯
ls -l tests/ovstest
# 如果不存在，重新編譯
make tests/ovstest
```

### Q2: 看不到日誌輸出

```bash
# 確保沒有重定向錯誤輸出
./tests/ovstest test-dt-lazy 2>&1 | cat

# 或直接查看標準錯誤
./tests/ovstest test-dt-lazy
```

### Q3: 後端切換不生效

```bash
# 檢查環境變量
echo $OVS_CLASSIFIER_BACKEND

# 在同一命令中設置和運行
OVS_CLASSIFIER_BACKEND=dt ./tests/ovstest test-dt-lazy
```

### Q4: DT 模式崩潰

```bash
# 使用 gdb 調試
gdb --args ./tests/ovstest test-dt-lazy

# 在 gdb 中
(gdb) run
# 如果崩潰
(gdb) bt  # 查看調用堆棧
```

---

## 驗證清單

運行以下命令確保整合成功：

- [ ] `make lib/libopenvswitch.la` 編譯成功
- [ ] `make tests/ovstest` 編譯成功
- [ ] `./test-integration.sh` 所有測試通過
- [ ] `unset OVS_CLASSIFIER_BACKEND && ./tests/ovstest test-dt-lazy` 通過（TSS）
- [ ] `export OVS_CLASSIFIER_BACKEND=dt && ./tests/ovstest test-dt-lazy` 通過（DT）
- [ ] 可以看到 "Using X classifier backend" 日誌
- [ ] 兩種模式下規則插入/查找/刪除都正常

---

## 進階測試

### 1. 壓力測試

```bash
# 創建大量規則測試
export OVS_CLASSIFIER_BACKEND=dt

# 修改測試程序參數（需要修改源碼）
# 或使用循環插入大量規則
```

### 2. 並發測試

```bash
# 多線程查找測試（需要額外實現）
# 確保 RCU 保護正確
```

### 3. 長時間運行測試

```bash
# 運行一段時間檢查內存洩漏
valgrind --leak-check=full ./tests/ovstest test-dt-lazy
```

---

## 測試報告模板

測試完成後，填寫以下報告：

```
測試日期：________
測試人員：________
OVS 版本：________

| 測試項目 | TSS 結果 | DT 結果 | 備註 |
|---------|---------|---------|------|
| 基本功能 | ✅/❌   | ✅/❌   |      |
| 規則插入 | ✅/❌   | ✅/❌   |      |
| 規則查找 | ✅/❌   | ✅/❌   |      |
| 規則刪除 | ✅/❌   | ✅/❌   |      |
| 後端切換 | ✅/❌   | ✅/❌   |      |
| 內存使用 | __MB   | __MB   |      |
| 查找時間 | __ms   | __ms   |      |

問題記錄：
1. 
2. 

建議：
1. 
2. 
```

---

## 資源

- **整合報告**: `DT_INTEGRATION_COMPLETE.md`
- **設計文檔**: `DT_INTEGRATION_DESIGN.md`
- **測試腳本**: `test-integration.sh`
- **DT 實現**: `lib/dt-classifier.c`
- **整合代碼**: `lib/classifier.c`, `ofproto/ofproto.c`

---

**記住**: 
- 默認使用 TSS，完全向後兼容
- DT 是實驗性功能，需要顯式啟用
- 遇到問題請查看日誌輸出
