# OVS Classifier - DT/TSS 整合設計

**日期**: 2025-10-16  
**目標**: 在 OVS classifier 中添加 Decision Tree 作為可選後端，保留 TSS 為默認

---

## 🎯 設計目標

1. **向後兼容** - 默認使用 TSS，現有代碼無需修改
2. **運行時切換** - 可通過配置選擇 DT 或 TSS
3. **接口統一** - 對外接口保持不變
4. **性能測試** - 可方便對比兩種實現

---

## 📐 架構設計

### 方案 A：編譯時選擇（簡單但不靈活）

```c
// classifier.h
#ifdef USE_DECISION_TREE
#include "dt-classifier.h"
#define CLASSIFIER_BACKEND "Decision Tree"
#else
#define CLASSIFIER_BACKEND "TSS (Tuple Space Search)"
#endif

struct classifier {
#ifdef USE_DECISION_TREE
    struct decision_tree dt;
#else
    // 現有 TSS 字段
    int n_rules;
    struct cmap subtables_map;
    struct pvector subtables;
    // ...
#endif
};
```

**優點**: 
- 實現簡單
- 零運行時開銷

**缺點**:
- 無法運行時切換
- 難以性能對比

### 方案 B：運行時選擇（推薦）

```c
// classifier.h
enum classifier_backend_type {
    CLASSIFIER_BACKEND_TSS,  // 默認
    CLASSIFIER_BACKEND_DT,   // Decision Tree
};

struct classifier {
    enum classifier_backend_type backend_type;
    
    union {
        /* TSS backend */
        struct {
            int n_rules;
            struct cmap subtables_map;
            struct pvector subtables;
            struct cmap partitions;
            struct cls_trie tries[CLS_MAX_TRIES];
            atomic_uint32_t n_tries;
        } tss;
        
        /* DT backend */
        struct decision_tree dt;
    } backend;
    
    bool publish;  // 共用字段
};
```

**優點**:
- 靈活切換
- 便於性能測試
- 可同時支持兩種實現

**缺點**:
- 實現稍複雜
- 需要分發函數調用

### 方案 C：策略模式（最靈活但最複雜）

```c
struct classifier_ops {
    void (*init)(struct classifier *);
    void (*destroy)(struct classifier *);
    void (*insert)(struct classifier *, const struct cls_rule *, ovs_version_t);
    const struct cls_rule *(*lookup)(const struct classifier *, ovs_version_t,
                                     const struct flow *, struct flow_wildcards *);
    bool (*remove)(struct classifier *, const struct cls_rule *);
    // ...
};

struct classifier {
    const struct classifier_ops *ops;
    void *backend_data;  // TSS 或 DT 的數據
    bool publish;
};

extern const struct classifier_ops tss_ops;
extern const struct classifier_ops dt_ops;
```

**優點**:
- 最靈活
- 易於添加新後端
- 清晰的接口分離

**缺點**:
- 實現複雜
- 間接調用有性能開銷

---

## 💡 推薦方案：混合方案 B+

**核心思想**: 使用方案 B 的結構，但添加編譯選項來完全禁用不需要的後端

```c
// config.h 或 configure.ac
#define ENABLE_TSS_BACKEND 1   // 默認啟用
#define ENABLE_DT_BACKEND 1    // 可選啟用

// classifier.h
enum classifier_backend_type {
#if ENABLE_TSS_BACKEND
    CLASSIFIER_BACKEND_TSS = 0,
#endif
#if ENABLE_DT_BACKEND
    CLASSIFIER_BACKEND_DT = 1,
#endif
    CLASSIFIER_BACKEND_DEFAULT = 0  // TSS
};

struct classifier {
    enum classifier_backend_type backend_type;
    
    union {
#if ENABLE_TSS_BACKEND
        struct {
            int n_rules;
            struct cmap subtables_map;
            struct pvector subtables;
            struct cmap partitions;
            struct cls_trie tries[CLS_MAX_TRIES];
            atomic_uint32_t n_tries;
        } tss;
#endif
        
#if ENABLE_DT_BACKEND
        struct decision_tree dt;
#endif
    } backend;
    
    bool publish;
};
```

---

## 🔧 實現步驟

### 階段 1：準備工作 ✅

- [x] 實現 DT 核心功能
- [x] 通過基礎測試
- [x] 確保接口穩定

### 階段 2：整合結構修改

#### 2.1 修改 classifier.h

```c
/* 在文件開頭添加 */
#include "dt-classifier.h"

/* 在 struct classifier 定義前添加 */
enum classifier_backend_type {
    CLASSIFIER_BACKEND_TSS = 0,   /* Tuple Space Search (default) */
    CLASSIFIER_BACKEND_DT = 1,    /* Decision Tree */
};

/* 修改 struct classifier */
struct classifier {
    enum classifier_backend_type backend_type;
    
    union {
        /* TSS backend (original implementation) */
        struct {
            int n_rules;
            uint8_t n_flow_segments;
            uint8_t flow_segments[CLS_MAX_INDICES];
            struct cmap subtables_map;
            struct pvector subtables;
            struct cmap partitions;
            struct cls_trie tries[CLS_MAX_TRIES];
            atomic_uint32_t n_tries;
        } tss;
        
        /* DT backend (new implementation) */
        struct decision_tree dt;
    } backend;
    
    bool publish;
};
```

#### 2.2 添加後端選擇函數

```c
/* classifier.h */
void classifier_set_backend(struct classifier *, enum classifier_backend_type);
enum classifier_backend_type classifier_get_backend(const struct classifier *);
const char *classifier_backend_name(enum classifier_backend_type);

/* classifier.c */
void
classifier_set_backend(struct classifier *cls, enum classifier_backend_type type)
{
    ovs_assert(classifier_is_empty(cls));
    cls->backend_type = type;
}

enum classifier_backend_type
classifier_get_backend(const struct classifier *cls)
{
    return cls->backend_type;
}

const char *
classifier_backend_name(enum classifier_backend_type type)
{
    switch (type) {
    case CLASSIFIER_BACKEND_TSS:
        return "TSS (Tuple Space Search)";
    case CLASSIFIER_BACKEND_DT:
        return "Decision Tree";
    default:
        return "Unknown";
    }
}
```

### 階段 3：修改 classifier 函數

#### 3.1 classifier_init()

```c
void
classifier_init(struct classifier *cls, const uint8_t *flow_segments)
{
    cls->backend_type = CLASSIFIER_BACKEND_TSS;  // 默認使用 TSS
    cls->publish = true;
    
    switch (cls->backend_type) {
    case CLASSIFIER_BACKEND_TSS:
        cls->backend.tss.n_rules = 0;
        cls->backend.tss.n_flow_segments = 
            flow_segments ? array_size(flow_segments) : 0;
        if (flow_segments) {
            memcpy(cls->backend.tss.flow_segments, flow_segments,
                   cls->backend.tss.n_flow_segments);
        }
        cmap_init(&cls->backend.tss.subtables_map);
        pvector_init(&cls->backend.tss.subtables);
        cmap_init(&cls->backend.tss.partitions);
        atomic_init(&cls->backend.tss.n_tries, 0);
        break;
        
    case CLASSIFIER_BACKEND_DT:
        dt_init(&cls->backend.dt);
        break;
        
    default:
        OVS_NOT_REACHED();
    }
}
```

#### 3.2 classifier_lookup()

```c
const struct cls_rule *
classifier_lookup(const struct classifier *cls, ovs_version_t version,
                  const struct flow *flow, struct flow_wildcards *wc)
{
    switch (cls->backend_type) {
    case CLASSIFIER_BACKEND_TSS:
        return classifier_lookup_tss(cls, version, flow, wc);
        
    case CLASSIFIER_BACKEND_DT:
        return dt_lookup(&cls->backend.dt, version, flow, wc);
        
    default:
        OVS_NOT_REACHED();
    }
}

/* 將原來的 classifier_lookup 重命名為 classifier_lookup_tss */
static const struct cls_rule *
classifier_lookup_tss(const struct classifier *cls, ovs_version_t version,
                      const struct flow *flow, struct flow_wildcards *wc)
{
    /* 原來的 TSS 實現 */
    const struct cls_subtable *subtable;
    const struct cls_rule *best;
    
    best = NULL;
    if (wc) {
        flow_wildcards_init_catchall(wc);
    }
    
    PVECTOR_FOR_EACH (subtable, &cls->backend.tss.subtables) {
        // ...原來的代碼...
    }
    
    return best;
}
```

#### 3.3 classifier_insert()

```c
void
classifier_insert(struct classifier *cls, const struct cls_rule *rule,
                  ovs_version_t version,
                  const struct cls_conjunction conj[], size_t n_conj)
{
    switch (cls->backend_type) {
    case CLASSIFIER_BACKEND_TSS:
        classifier_insert_tss(cls, rule, version, conj, n_conj);
        break;
        
    case CLASSIFIER_BACKEND_DT:
        /* DT 暫不支持 conjunction */
        ovs_assert(n_conj == 0);
        dt_insert_rule(&cls->backend.dt, rule, version);
        break;
        
    default:
        OVS_NOT_REACHED();
    }
}
```

### 階段 4：測試與驗證

#### 4.1 單元測試

```c
/* tests/test-classifier.c */
static void
test_backend_switching(void)
{
    struct classifier cls;
    
    /* 測試 TSS */
    classifier_init(&cls, NULL);
    assert(classifier_get_backend(&cls) == CLASSIFIER_BACKEND_TSS);
    
    /* 添加一些規則 */
    // ...
    
    /* 測試查找 */
    // ...
    
    classifier_destroy(&cls);
    
    /* 測試 DT */
    classifier_init(&cls, NULL);
    classifier_set_backend(&cls, CLASSIFIER_BACKEND_DT);
    assert(classifier_get_backend(&cls) == CLASSIFIER_BACKEND_DT);
    
    /* 添加相同規則 */
    // ...
    
    /* 驗證結果一致 */
    // ...
    
    classifier_destroy(&cls);
}
```

#### 4.2 性能測試

```c
/* utilities/ovs-benchmark.c */
static void
benchmark_classifier_backends(int n_rules)
{
    struct classifier cls_tss, cls_dt;
    struct timeval start, end;
    long tss_time, dt_time;
    
    /* 準備測試數據 */
    struct cls_rule *rules[n_rules];
    // ...生成規則...
    
    /* 測試 TSS */
    classifier_init(&cls_tss, NULL);
    gettimeofday(&start, NULL);
    
    for (int i = 0; i < n_rules; i++) {
        classifier_insert(&cls_tss, rules[i], 0, NULL, 0);
    }
    
    gettimeofday(&end, NULL);
    tss_time = timeval_diff(&end, &start);
    
    /* 測試 DT */
    classifier_init(&cls_dt, NULL);
    classifier_set_backend(&cls_dt, CLASSIFIER_BACKEND_DT);
    gettimeofday(&start, NULL);
    
    for (int i = 0; i < n_rules; i++) {
        classifier_insert(&cls_dt, rules[i], 0, NULL, 0);
    }
    
    gettimeofday(&end, NULL);
    dt_time = timeval_diff(&end, &start);
    
    printf("Insertion time (%d rules):\n", n_rules);
    printf("  TSS: %ld ms\n", tss_time);
    printf("  DT:  %ld ms\n", dt_time);
    printf("  Speedup: %.2fx\n", (double)tss_time / dt_time);
    
    // ...查找性能測試...
    
    classifier_destroy(&cls_tss);
    classifier_destroy(&cls_dt);
}
```

---

## 🎚️ 配置選項

### 編譯時選項

```bash
# configure 添加選項
./configure --enable-dt-classifier  # 啟用 DT 後端
./configure --disable-tss-classifier  # 禁用 TSS（節省空間）
```

### 運行時選項

```bash
# OVS 配置文件或環境變量
export OVS_CLASSIFIER_BACKEND=dt   # 使用 DT
export OVS_CLASSIFIER_BACKEND=tss  # 使用 TSS (默認)

# ovs-vsctl 命令
ovs-vsctl set Open_vSwitch . other_config:classifier-backend=dt
```

---

## 📊 遷移計劃

### 第一階段：實驗性支持（當前）
- ✅ DT 獨立實現完成
- ⏳ 整合到 classifier 結構
- ⏳ 基礎功能測試

### 第二階段：功能完善
- 添加 conjunction match 支持
- 完整的 wildcard 追蹤
- 與 TSS 功能對等

### 第三階段：性能優化
- 樹構建策略優化
- 大規模測試
- 性能調優

### 第四階段：生產就緒
- 穩定性測試
- 文檔完善
- 默認啟用（可選）

---

## ⚠️ 注意事項

### 當前限制

1. **Conjunction 支持**
   - DT 暫不支持 conjunction match
   - 需要添加專門的處理邏輯

2. **Wildcard 追蹤**
   - 基本框架已實現
   - 需要完善細節

3. **Trie 索引**
   - TSS 使用 prefix trie 加速
   - DT 已經是樹結構，不需要額外 trie

4. **Partition**
   - TSS 使用 partition 優化
   - DT 需要評估是否需要

### 兼容性

- **向後兼容**: 默認 TSS，現有代碼無需改動
- **接口兼容**: 所有 classifier_* 函數保持不變
- **數據兼容**: 規則格式完全相同

---

## 🔬 測試策略

### 正確性測試
- [ ] 所有現有 classifier 測試用例通過
- [ ] TSS 和 DT 結果一致性測試
- [ ] 邊界條件測試

### 性能測試
- [ ] 小規模（<100 規則）
- [ ] 中規模（100-1000 規則）
- [ ] 大規模（>1000 規則）
- [ ] 查找延遲測試
- [ ] 插入/刪除性能

### 穩定性測試
- [ ] 內存洩漏檢查（valgrind）
- [ ] 並發壓力測試
- [ ] 長時間運行測試

---

## 📝 文件修改清單

### 必須修改
- `lib/classifier.h` - 添加後端類型和聯合體
- `lib/classifier.c` - 添加分發邏輯
- `lib/automake.mk` - 添加 dt-classifier.c 到編譯

### 可選修改
- `configure.ac` - 添加 --enable-dt-classifier 選項
- `vswitchd/bridge.c` - 添加配置讀取
- `utilities/ovs-benchmark.c` - 添加性能測試
- `tests/test-classifier.c` - 添加後端測試

### 新增文件
- `lib/dt-classifier.h` ✅ (已存在)
- `lib/dt-classifier.c` ✅ (已存在)
- `tests/test-dt-classifier.c` (可選)

---

**下一步**: 開始實施階段 2 - 修改 classifier.h 和 classifier.c
