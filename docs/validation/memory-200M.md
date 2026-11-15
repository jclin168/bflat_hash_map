# 200M RSS / VM：採用 reserve +12.5%

bflat 使用任意 group 數與分級成長，工作負載設定為 `reserve(N + N/8 + (N%8 != 0))`。reserve 預留由呼叫端控制，探測、插入與 anti-drift 採用容器預設策略。

200M uint64 key → uint64 value；線上產生雙射唯一 key，const pair& emplace，無 O(N) 輸入陣列。每容器兩種 reserve：200M 對照與 225M 採用方案。填滿後完成公開可用的 migration，接著做 20M 次先插入再刪除，活元素維持 200M；刪除不同的原始 key，新 key 不重複。最後再次完成 migration。每配置一個新程序、單次記憶體測試、依序執行、CPU 0。

Boost 1.92；sflat 使用 pair-emplace 最佳化版本；bflat SHA256 84c0d8a82f094b41f726b47053a82190c5932a58c59e24fdec1d9c87783ca16d。GCC 14.2、C++17、-O3 -DNDEBUG -march=native、SFLAT_PROCESS_SEED=1。8 GiB cgroup，無 swap。

五配置完整完成，均檢查插入成功、20M 次插入／刪除數量、最終 size 與 key/value 抽樣。Boost reserve(225M) 跨容量級距，在 RSS 超過測試設定的 7 GiB 時停止，並非配置已完成或發生 OOM。其結果分開記錄。RSS 取 smaps_rollup，VM 取 /proc/status，GiB = 2^30 bytes，未扣程序 baseline。峰值合併 VmHWM/getrusage、VmPeak 與外部監測，RSS 與 VM 峰值不保證同時發生。VM 是位址保留，不等於實體記憶體用量。

## 完成後用量與全程峰值（GiB）

| reserve | 容器 | 填滿 RSS | 填滿 VM | churn 後 RSS | churn 後 VM | RSS 峰值 | VmPeak |
|---|---|---:|---:|---:|---:|---:|---:|
| 200M | boost | 4.002 | 4.006 | 4.002 | 4.006 | 4.002 | 4.006 |
| 200M | sflat | 3.648 | 3.652 | 3.648 | 3.652 | 3.648 | 3.654 |
| 200M | bflat | 3.637 | 3.641 | 4.545 | 4.549 | 4.775 | 8.186 |
| 225M | sflat | 4.105 | 4.109 | 4.105 | 4.109 | 4.105 | 4.111 |
| 225M | bflat | 4.090 | 4.094 | 4.090 | 4.094 | 4.090 | 4.096 |

## 未完成的補充對照

boost reserve(225M)：實際容量 503,316,479；已確認插入 3,961,856 筆。RSS 觀測峰值 7.004 GiB，VmPeak 8.006 GiB。停止原因 rss_watchdog_7GiB，不可視為容器最大可插入數。


## 實際容量

| reserve | 容器 | 填滿容量 | churn 後容量 | 最終填充率 |
|---|---|---:|---:|---:|
| 200M | boost | 251,658,239 | 251,658,239 | 0.794729 |
| 200M | sflat | 228,571,456 | 228,571,456 | 0.875000 |
| 200M | bflat | 228,571,439 | 285,714,299 | 0.700000 |
| 225M | sflat | 257,142,880 | 257,142,880 | 0.777778 |
| 225M | bflat | 257,142,869 | 257,142,869 | 0.777778 |

## 解讀限制與重現

這是特定筆數與 20M churn 的單次記憶體評估，非耗時或延遲 benchmark，也不代表任意長 churn 永遠不需 rehash。採用額外 reserve 後，bflat 仍保留原本 anti-drift，耗盡時仍可能同容量重建。Boost 的容量取整可能使相同 reserve 增幅跨級，需看實際容量。

此資料使用任意 group 數版本，不與其他容量策略的結果合併。memory.png/svg 僅畫此資料集；x=0..200 是填入筆數，200..220 是後續 churn pairs。summary.json/csv、raw CSV 與外部監測樣本保留完整數字；memory.cpp/run.py/commands.json/environment.json 提供重現資訊。先執行 python3 run.py，再執行 python3 report.py；Boost include 路徑需按環境調整。
