# DT Classifier - 快速命令參考

## 編譯命令

```bash
# 重新編譯 dt-classifier
cd /mnt/d/ovs_DS
make lib/dt-classifier.lo

# 編譯測試程序
./libtool --mode=link gcc -o dt-test \
    lib/dt-classifier-test.c \
    lib/dt-classifier.lo \
    lib/.libs/libopenvswitch.a \
    -I. -Iinclude \
    -lpthread -lrt -lm -lssl -lcrypto -lcap-ng
```

## 測試命令

```bash
# 運行最小測試
./dt-test-minimal

# 運行完整測試
./dt-test

# 運行調試測試
./dt-test-ultra-simple

# 運行自動化測試腳本
./run-dt-tests.sh
```

## 檢查命令

```bash
# 查看編譯產物
ls -lh lib/dt-classifier.*

# 查看符號表
nm lib/dt-classifier.o | grep " T "

# 查看未定義符號
nm lib/dt-classifier.o | grep " U "

# 檢查編譯警告
make lib/dt-classifier.lo 2>&1 | grep "warning"
```

## 清理命令

```bash
# 清理編譯產物
make clean

# 只清理 dt-classifier
rm -f lib/dt-classifier.o lib/dt-classifier.lo

# 清理測試程序
rm -f dt-test dt-test-minimal dt-test-ultra-simple
```

## 在 Windows PowerShell 中使用

```powershell
# 編譯
wsl bash -c "cd /mnt/d/ovs_DS && make lib/dt-classifier.lo"

# 測試
wsl bash -c "cd /mnt/d/ovs_DS && ./dt-test"

# 運行測試腳本
wsl bash -c "cd /mnt/d/ovs_DS && ./run-dt-tests.sh"
```

## 常見問題

### Q: 編譯失敗怎麼辦？

```bash
# 1. 檢查錯誤信息
make lib/dt-classifier.lo 2>&1 | less

# 2. 確保頭文件正確
grep "#include" lib/dt-classifier.c

# 3. 重新生成依賴
./boot.sh && ./configure
```

### Q: 測試失敗怎麼辦？

```bash
# 1. 運行調試測試
./dt-test-ultra-simple

# 2. 使用 gdb 調試
gdb ./dt-test
(gdb) run
(gdb) bt

# 3. 查看日志
./dt-test 2>&1 | tee test.log
```

### Q: 如何添加新測試？

```c
// 在 lib/dt-classifier-test.c 中添加:
static void
test_my_new_feature(void)
{
    struct decision_tree dt;
    dt_init(&dt);
    
    // 測試代碼...
    
    printf("My test: %s\n", result ? "PASS" : "FAIL");
    
    dt_destroy(&dt);
}

// 在 main() 中調用:
test_my_new_feature();
```

## 項目文件結構

```
d:\ovs_DS\
├── lib/
│   ├── dt-classifier.h              公開接口
│   ├── dt-classifier.c              實現
│   ├── dt-classifier-test.c         完整測試
│   ├── dt-test-minimal.c            基礎測試
│   └── dt-test-ultra-simple.c       調試測試
├── run-dt-tests.sh                   自動化測試
├── DT_CLASSIFIER_README.md           設計文檔
├── DT_CLASSIFIER_QUICKSTART.md       快速入門
├── DT_PROGRESS_REPORT.md             進度報告
├── DT_SUCCESS_MILESTONE.md           成功里程碑
└── DT_QUICK_REFERENCE.md             本文檔
```
