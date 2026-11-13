# 串流診斷

診斷預設關閉。`MSU2_DIAGNOSTICS=1` 記摘要，`=2` 另記每次 payload 寫入時間與長度。透過 `open` 啟動時可用 `--env` 明確指定新實例的環境；已執行的程序不會因另一個 shell 改變變數而更新。啟動範例與核實方式見[架構](ARCHITECTURE_zh-TW.md)。

## 收集與完整性

從 App 的「工作紀錄」取得本次 Controller 的資料夾。核對實際 App 路徑、GUI／CLI SHA256、來源 commit（如可得）、啟動環境與測試設定，再收集：

| 檔案 | 用途 |
|---|---|
| `events.log` | 連線、工作、錯誤與診斷啟用狀態 |
| `stream_N.csv` | 來源幀、處理、區域、傳輸時間與 bytes |
| `diagnostics.csv` | 階段事件及 RX 分類；確認 `log,begin` 存在 |

Controller 正常解構後才會寫出 `log,end`。沒有結束列時視為不完整；lost 或 write_failed 非零時，分析要保留遺失／失敗範圍。異常終止可能遺失尾端事件，不能把提前複製的檔案當成最終日誌。

## 欄位與限制

- 記錄幀／patch 編號、矩形、bytes、時間、佇列取代與握手／ACK／寫入階段。完整契約在 [DiagnosticLog](../msu2_macos_app/src/diagnostic_log.hpp) 與 [TraceEvent](../msu2_linux_v0_3/include/msu2/trace.hpp)。
- `rxclass` 只分類既有 read 中的 banner、MSNCN 與 `02 03`，可跨 read；分類不改 ACK 接受規則。`rxread` 最多保留一次等待的前 32 筆非空 read 時間／長度，超限另計，單筆即成功時可省略。
- 不保存桌面像素、Pair 顏色、payload 或未知 RX 原始內容。收到 bytes 不代表收到 ACK；已知樣式計數為零也不能證明韌體重啟。
- 間隔用單調時鐘，牆鐘錨點僅精確到秒。`captured` 是 `Source::get` 返回時間，非 ScreenCaptureKit PTS；桌面靜止時可能重複回傳快取影像，佇列年齡會低估內容年齡。
- 背景 writer 使用有界緩衝，但 record 仍有鎖與複製成本。診斷不是零成本；比較效能需保留相同條件的關閉對照，等級 2 不宜作一般效能基準。

寫檔失敗與緩衝溢位的安全注入仍待驗證；Pair24／28 實機逾時仍未定位。結果與下一步以[現況](STATUS_zh-TW.md)為準，速率的意義見[量測定義](../msu2_macos_app/docs/STREAM_METRICS_zh-TW.md)。


## 02 離線候選：來源 metadata 與 callback 代理

CSV header 與既有事件欄位定義不變；新增以下事件，嚴格 stage 白名單讀取器需更新。三種事件 rx_len=0、rx_hex 空；不含影像或原始 RX。frame 是 producer 的排程序號，並非來源序號。

| kind/stage | result | v0..v5（未使用為0） |
|---|---|---|
| capture/identity | available/unavailable | 程序內 capture epoch、來源 sequence、producer 重讀快取(-1 unavailable/0新樣本/1重讀)、callback steady-clock **奈秒**絕對值、0、0 |
| capture/pts_opaque | numeric/invalid/unavailable | CMTime value、timescale、epoch、原 flags、0、0；不做 age |
| frame/callback_to_host | ok/invalid/unavailable | callback 到 worker 取幀代理微秒（無效=-1）、epoch、sequence、producer重讀標記、0、0 |

epoch 僅在當前程序內唯一，合併程序日誌需加 session 身分；sequence 每次接受並發布影像遞增，idle callback 不遞增。image/metadata 同鎖發布與複製；相同像素的新來源樣本仍是新 sequence。重讀標記相對前次 producer 讀取，不是前次實際傳送；queue replacement 不改其意義。

callback 時刻在 SCK delegate 入口採樣，與主機 trace 使用同一 steady_clock；它包含影像轉換、等待與佇列延遲，但不包含回呼之前的擷取/交付延遲。既有 frame/take v0 保持 Source::get 返回後的 queue-age。兩者都不是精確內容年齡，LCD 年齡未量測。PTS 時鐘域未證實，不與 host 相減；本輪不加入 displayTime。

來源無 metadata（圖片、合成或既有 fake source）明確記 unavailable，不能把零值當新鮮影格。callback 非正值或在 host 的未來時記 invalid；有效 callback age 不依賴 PTS 有效性。診斷關閉不產生新增事件；仍有 callback 時鐘讀取及固定大小 metadata 更新/傳遞成本，尚無效能量測。開啟後每次 producer 讀取多兩事件、每次 worker 取幀多一事件，需核對 lost 計數，不能宣稱不增加負載。

新增 capture 事件在 queue.put 後記錄，跨執行緒可能晚於對應 frame 事件寫入。解析需依 session、stream 邊界與 frame id 關聯，不能要求 CSV 中相鄰或固定先後；frame id 在每次 stream 會重設。
