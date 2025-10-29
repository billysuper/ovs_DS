# 編譯測試套件的步驟

## 問題診斷

當前遇到的編譯問題：
1. 缺少 `.deps/*.Po` 依賴文件 - 已解決（通過touch創建）
2. 缺少 `odp-netlink.h` 頭文件 - 需要先編譯OVS主庫

## 解決方案

### 方法 1: 完整編譯 OVS (推薦)

```bash
cd /mnt/d/ovs_DS

# 步驟 1: 創建缺少的依賴文件（如果需要）
while make 2>&1 | grep -q "No such file.*\.Po"; do 
    missing=$(make 2>&1 | grep "No such file" | awk '{print $2}' | head -1 | tr -d ':')
    touch "$missing"
    echo "Created $missing"
done

# 步驟 2: 編譯整個 OVS 專案（這會生成所有需要的頭文件）
make -j4

# 步驟 3: 編譯測試
make tests/ovstest

# 步驟 4: 運行決策樹測試
./tests/ovstest test-dt-classifier
```

### 方法 2: 只編譯必要的庫

```bash
cd /mnt/d/ovs_DS

# 編譯庫文件（生成頭文件）
make lib/libopenvswitch.la

# 然後編譯測試
make tests/ovstest
```

### 方法 3: 如果上述方法都失敗

可能需要重新配置構建系統：

```bash
cd /mnt/d/ovs_DS

# 清理
make distclean 2>/dev/null || true

# 重新配置
./configure

# 編譯
make -j4
make tests/ovstest
```

## 運行測試

編譯成功後：

```bash
# 運行所有決策樹測試
./tests/ovstest test-dt-classifier

# 運行特定測試
./tests/ovstest test-dt-classifier empty
./tests/ovstest test-dt-classifier dual
./tests/ovstest test-dt-classifier benchmark
```

## 常見錯誤

### 錯誤 1: No rule to make target 'tests/.deps/xxx.Po'
**解決**: 
```bash
touch tests/.deps/xxx.Po
```

### 錯誤 2: fatal error: odp-netlink.h: No such file or directory
**原因**: 尚未編譯生成這個頭文件
**解決**: 先運行 `make lib/libopenvswitch.la` 或 `make`

### 錯誤 3: configure: error: Python 3.4 or later is required
**解決**: 確保安裝了 Python 3.4+
```bash
sudo apt-get install python3 python3-dev
```

## 預期結果

編譯成功後應該看到：
```
gcc ... -o tests/ovstest ...
```

運行測試成功後應該看到：
```
=== Running Decision Tree Classifier Tests ===

PASSED: empty tree test
PASSED: single rule test
PASSED: priority ordering test
...
=== All Tests Passed ===
```

## 目前狀態

已完成：
- ✅ 創建測試文件 `tests/test-dt-classifier.c`
- ✅ 更新 `tests/automake.mk` 加入編譯規則
- ✅ 創建 Autotest 定義 `tests/dt-classifier.at`
- ✅ 創建所需的 `.Po` 依賴文件

待完成：
- ⏳ 生成 `odp-netlink.h` (通過編譯主庫)
- ⏳ 編譯測試可執行檔
- ⏳ 運行測試驗證決策樹正確性

## 下一步

推薦執行：
```bash
cd /mnt/d/ovs_DS
make lib/libopenvswitch.la
```

這個命令會生成所有必要的頭文件，之後就可以成功編譯測試了。
