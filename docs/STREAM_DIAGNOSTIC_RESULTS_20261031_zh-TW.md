# 桌面串流診斷：2026-10-31

使用 `hardware_candidate_v3/MSU2 Mini Studio.app` 的實機紀錄；Raw 與 Pair 不是同一個受控桌面場景，且中間重新插拔過裝置，不能用單次測試判定兩種編碼的因果差異。`host` 傳送完成、ACK 和 LCD 實際顯示是不同事件。

## Raw 全幅，目標 28 FPS

證據：`results/macos/stream_trace_20261030/gui_resume_20261031/`。真實桌面、Raw、無差分、priority off、chunk 60、timeout 3000 ms、未自動重連；約 19.3 秒成功送出 469 幀，主機更新約 24.35 次／秒。來源取得約 28.02 次／秒、capture slots missed 0、等待佇列被取代 70 次。

每幀處理時間中位數約 41.03 ms，payload 寫入約 39.88 ms；window ACK 等待中位數約 0.86 ms、p95 約 1.43 ms。每次 payload 約 39,000 bytes。這個場景中的桌面幾乎靜止，數字是主機傳輸量測，不代表 LCD FPS 或動態桌面時效。

診斷檔有正常 `log,end`，記錄 4,318 個事件，緩衝遺失及寫入失敗均為 0。下一次連線在 Pair 傳圖前發生 handshake echo timeout：發出握手後 3 秒內收到 6 bytes，但沒有匹配 MSNCN；未保存未識別資料的內容，不能判定它是什麼。

## 重新插拔後：Pair 全幅，目標 28 FPS

證據：`results/macos/stream_trace_20261030/gui_replug_20261031/`。握手成功後送出 50 幀，第 50 號影格等待 window ACK 時逾時；成功影格傳送期間約 27.6–27.7 次／秒，整段包含 3 秒等待不可當作正常傳送速率。來源取得約 28.07 次／秒，等待佇列的 83 次取代發生在 ACK 等待期間，失敗前已取用幀的 ID 連續。

成功幀 payload 多為約 29,034 bytes；寫入時間中位數約 33.17 ms，ACK 等待中位數約 2.48 ms。失敗前最後一幀 payload 寫入約 38.32 ms；下一次 window 命令後等待 3,000.5 ms，收到 210 bytes、25 次讀取，但未匹配 `02 03` ACK 前綴。記錄沒有保存未識別資料內容，所以無法確認資料是否為 banner、其他協定訊息或其他輸入；也無法證明裝置重啟或 Pair 為根因。

診斷檔有 `log,end`，written 557、lost 0、write_failed 0。此次 Pair 29 與診斷關閉對照因回合失敗而未執行。GUI 與 CLI 執行檔前後 SHA256 相同；測試結束後裝置別名和共用鎖均無已列出的擁有者。

## 後續

已補充只根據既有 read 結果統計 banner／MSNCN／ACK 前綴次數與有限筆讀取時間、長度的診斷；不保存未知原始 bytes，不改協定或增加讀取。完整離線套件、ASan／UBSan、獨立建置、簽章及共用鎖路徑驗證通過。新候選在 `results/macos/stream_trace_20261030/hardware_candidate_rxclass/MSU2 Mini Studio.app`，目前仍需確認其新簽章的桌面擷取授權，尚未以此版本實機串流。額外重跑 Raw 曾被自動審查拒絕，理由是視為既定矩陣外再次傳送桌面資料；現有 Raw 證據保留。
