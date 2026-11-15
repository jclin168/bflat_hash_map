# bflat：Ankerl fuzz corpus 相容性驗證

使用採用的任意 group 數／tiered 成長 bflat，工作負載 reserve +12.5%。
完整 source 在 ../bflat-ankerl-compatible；原始接受版本 header 在 inputs-original。

## 測試來源與範圍

來源為 Martin Leitner-Ankerl 的 martinus/unordered_dense，commit
7c58a8b7a48e790fe72f6508eb3b46317cbba9d5：
- test/unit/fuzz_insert_erase.cpp：117 個既存 corpus。
- test/unit/fuzz_string.cpp：246 個既存 corpus。
- test/fuzz/provider.h：保持原樣，既存 corpus 位元組保持原樣。

本次將上述兩個 workload 接入 bflat，保留原本的操作順序、hash、
std::unordered_map oracle 與全部 REQUIRE 判定。原始檔在 ankerl/original，
移植版在 ankerl/adapted。更改只有 Map 實例、reserve 測試設定、peak 計數與
啟動／斷言機制：REQUIRE 失敗改為拋例外並輸出 corpus 路徑，並非忽略斷言。
prepass 以標準容器跑相同 workload，取得活元素數峰值，再測不 reserve、
reserve(峰值)、reserve(峰值 + ceil(峰值/8))。沒有改寫 corpus 讓測試通過。

這是 Ankerl 兩個既存 fuzz 測項的 corpus replay，並非持續 coverage-guided
fuzzing，也未涵蓋 unordered_dense 的全部 API／實作專屬測試。
未找到獨立以 Hardness 命名的 Ankerl 套件，因此不以此名宣稱全套通過。

## 發現與修正

原版無法編譯 Ankerl dummy_hash 與 ankerl::unordered_dense::hash：
它們的 is_avalanching 型別是 void，但 bflat 嘗試讀取 void::value。
logs/ankerl-original-build.txt 記錄實際錯誤。相容性修正將 void 視為 true，
保留 true_type/false_type 的原本行為；容量、探測、插入、anti-drift 不變。
std::hash 的判斷也不變，前次 200M 記憶體結果使用 std::hash，沒有因此失效。

## 結果與重現

Release：363 檔案 × 3 reserve 模式 = 1,089 個 bflat 案例全部通過。
ASan/UBSan：相同 1,089 個案例全部通過，且強制 incremental migration、
每步一個 group、停用 mmap。沒有跳過失敗的 corpus。

額外的本機弱 hash 差分測試在 logs/release.txt、logs/sanitized.txt，
包含常數 hash、64 值 hash、未混合 13 值 hash、字串與成長；兩者均 PASS。
這些補充測試不計入 Ankerl corpus 案例數。

執行 ./ankerl/run.sh 重現；Ankerl header、provider 與 corpus 都已凍結在此目錄。
inputs 為相容性修正版，inputs-original 為修正前版本；亦保留原始授權。

來源連結：[Ankerl 整數 fuzz](https://github.com/martinus/unordered_dense/blob/7c58a8b7a48e790fe72f6508eb3b46317cbba9d5/test/unit/fuzz_insert_erase.cpp)、[字串 fuzz](https://github.com/martinus/unordered_dense/blob/7c58a8b7a48e790fe72f6508eb3b46317cbba9d5/test/unit/fuzz_string.cpp)。

相容性修正後，bflat 自身完整 12 組 unit-test 配置均通過，包含有／無 Boost 的 hash marker、API、全部成長級距、差分、強制漸進搬移、scalar/SSE2 與 ASan/UBSan。紀錄在 logs/bflat-full-unit-tests.txt。修正 patch 為 from-accepted-tiered.patch；合併上游原版變更為 from-upstream-7c24911.patch；完整 source 在 source.tar.gz。

PR 重現：從 repo 根目錄執行 `bash tests/ankerl/run.sh`。
