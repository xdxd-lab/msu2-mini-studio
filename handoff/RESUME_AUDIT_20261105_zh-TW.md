# 中斷核對與恢復工作

2026-11-05，p1 統籌、p2 測試、p3 分析共同核對。使用者本次明確允許核對後接續，已取代先前「沒有重新授權不得續跑 Pair24」的待命狀態。本文件不代表實測已完成。

## 中斷證據

p1 session 的 task_complete 錯誤紀錄（轉為台灣時間）：

- 10/30 02:49：usage_limit_exceeded，遠端壓縮工作中止。
- 10/30 03:09：usage_limit_exceeded，該次回合中止。
- 10/31 04:21：usage_limit_exceeded；Pair24 後續工作未見結果。
- 11/05 07:16：Fatal error: application network policy is unavailable，該次回合中止，錯誤分類僅為 other。

這些證據顯示代理工作回合確實中斷；不能據此斷言曾有一般網際網路斷線，或把 USB 的握手／ACK 失敗歸因為網路。Pair24 未產出結果由 p2、p3 及交接記憶交叉確認，但不能只由回合錯誤推知硬體曾開始或完成。

另有不同原因造成的停頓：新候選的螢幕擷取授權、實機 handshake echo/window ACK timeout，以及額外重跑 Raw 的自動審查拒絕。原始紀錄與既有測試成果保留。

## 本次接續範圍

使用現有 RX 分類候選，不改產品程式。p2 重新檢查擁有者與 guard、雜湊及桌面授權，做一次基準握手；成功才在同一連線做 Pair/full/priority off/24 FPS/trace1 約15秒。若ACK失敗，停止傳圖，在安全持有guard下做一次純握手探測後結束。Pair29、Raw重跑及其他測試暫不展開。所有新證據保存在 `results/macos/stream_trace_20261105/`。

已經隔數日，現在的基準或握手探測不能代表10/31失敗當下的設備狀態。鎖檔內舊pid不是活躍擁有者的證據，須檢查當前 lsof 與作業系統鎖。

分工仍是 p1 PM 統籌審核報告、p2 唯一硬體測試者、p3 分析與經核准的產品修改。p2 現行 pane 顯示 GPT-6-Luna medium（本輪觀察到切換），p1 沿用目前設定而未強制更換。

## 執行結果（已完成）

- p2 已確認凍結App的精確程序路徑、trace1、桌面預覽及guard，基準握手成功。Pair24 實際成功36幀／約1.494秒（23.99次／秒），第36號幀的window ACK等待約3000ms，讀到0 bytes後失敗。沒有重跑Raw或Pair29。
- 未重新插拔，關閉連線約28.2秒後重新開埠，banner與MSNCN握手成功；這不代表逾時當下已可回應，也不代表面板已恢復或可傳圖。
- p1發現初次複製的副本缺少log,end，已保留 `diagnostics_pre_exit.csv`／`events_pre_exit.log`。App正常結束後取得最終原始日誌：written470、lost_full0、lost_after_failure0、write_failed0。候選GUI／CLI前後SHA相符。
- p3已在 `results/macos/stream_trace_20261105/ANALYSIS_zh-TW.md` 定稿；操作與原始資料見同目錄 `run_notes.md`、CSV與events。Pair24仍失敗，削弱「僅因28FPS過高」假說，但尚不能歸因於Pair編碼或韌體。這次失敗前無寫入或ACK逐漸變慢，不能把10/31的現象當作必要前兆。
- 本輪沒有修改產品程式。接續測試已完成，後續新實驗應依證據安排範圍，避免無資訊增益的重複傳圖。
