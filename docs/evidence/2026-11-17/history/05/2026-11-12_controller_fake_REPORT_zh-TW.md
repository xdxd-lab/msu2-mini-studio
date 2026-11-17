# MSU2-05 Controller Priority 離線 Probe

## 範圍與方法

- 使用隔離 source `source/`（revision `fb3cfc3aa9494af4e6cdb49bb22b97a67a4405e4`）和 sibling `build/`；未修改產品原始碼或其他候選。
- Controller 設定：`source=desktop`, 60 FPS, `center3`, full Raw (`differential=false`, `pair=false`), chunk 60, timeout 3000 ms, hold 1 ms, auto reconnect 關閉；每個正式 case 執行 5 秒。
- `Dependencies.source` 提供自製 RGB888 畫面，fake lease 與 fake transport 提供正常握手及 ACK。`source=desktop` 只是設定值；依賴注入使 Controller 不建立 `ScreenCapture`，**沒有呼叫 SCK，也沒有擷取真桌面**。沒有使用硬體或 App。
- fake payload write 採累積 bytes deadline，約 420,000 bytes/s；不是每個 chunk 固定睡眠。ACK 立即回覆。影格來源輸出按 source frame ID 記錄 center/left/right 區域 RGB565 後的自訂 64-bit rolling fingerprint（非標準 FNV），沒有儲存畫面。
- 兩 case 共用中心持續變動、右區靜止；左區分別每個 source frame 變動，或以 `frame_id % 42` 週期變動。60 FPS 下 42 個來源時槽名義上約 700 ms；實際 elapsed 受真實 `steady_clock`、排程抖動和 queue replacement 影響。

## 正式結果

| Case | 完成 frame | sent / unchanged / skipped | center due/sent | left due / sent / skipped / missed queue | right due/unchanged | RGB565 fingerprint 數 center/left/right | 等待影格取代 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 連續左區變動 | 287 | 114 / 98 / 75 | 99 / 99 | 98 / 14 / 75 / 9 | 98 / 98 | 256 / 256 / 1 | 13 |
| 左區 42-slot 週期變動 | 289 | 114 / 98 / 77 | 99 / 99 | 98 / 14 / 77 / 7 | 98 / 98 | 256 / 42 / 1 | 11 |

兩次都由 full/off 首次傳送進入 `center3`，轉換次數為 1。5 秒窗口中左區的變動確實留下 RGB565 fingerprint 變化，且都有 14 次左區 priority-region send；**沒有重現左區零 send**。中心 slot 在觀察到的 99 次 due 全數送出。靜止右區的 98 個 slot 都是 unchanged；因此右區約 4.92 秒自上次 host send 的尾端時間表示沒有再傳送變動資料，不能解讀成右區飢餓或 LCD 沒更新。

診斷資料中的 frame-done wall busy sum 約 2.71–2.75 秒，最大單幀約 86 ms；本次傳輸總 bytes 約 2.108 MB。這些是 fake transport 與目前主機排程下的觀察值。

## 限制

- 沒有穩定 clock hook，因此無法做確定性 Q4 replay；所有 `t_us` 是真實 `steady_clock`。`diagnostics.csv` 沒有 cost-predicted 欄位，不能事後聲稱精確預測成本或精確解釋每次 skip 的 prediction。
- fake ACK 立即回覆，未模擬 ACK latency / timeout / 錯誤；fake transport 不等同 SCK、序列埠韌體或 LCD。host send / ACK 不代表面板已顯示。
- 連續左區公式的 8-bit source value 會循環；相鄰 source frames 仍變動，指紋顯示本窗口 256 種不同值。週期 case 明確以 42 個 source slots 為週期，不宣稱實際 wall-clock 每次恰為 700 ms。
- 對首個測試 executable 的第一次 targeted link 曾失敗，原因是未連結 `platform.o`，連結器報 `ScreenCapture`、`enumerate_devices`、`load_image`、`save_image_png` 符號未定義。補上 `platform.o` 後重連成功。那次原始失敗輸出沒有保存在最終 `commands.json`；`commands.json` 記錄的是成功的 targeted compile 和兩次正式執行，不能把失敗輸出說成仍已保存。

## Evidence

- `analysis.json`：逐區 due/outcome、last send gap 含尾端、source gap 與 frame fingerprint 摘要。
- `commands.json`：成功命令 argv、exit、elapsed、compile 前 source SHA，以及最後 executable / app helper SHA256。
- `logs/<run>/events.log`, `stream_1.csv`, `diagnostics.csv`：Controller 原始記錄；每個 case 的 fake metadata 與逐 source frame fingerprints 在 `logs/` 根目錄。
- `build/`：隔離 build objects、probe executable 和 build outputs。
- `probe/`：probe harness、runner 與 postprocessor。

## 審核追加：模型與證據邊界

全部234項基準來源/符號連結經再次核對仍與BASE.json一致。低負載對照未做；沒有修改產品或引入02 metadata。build.sh為取得物件曾產生新的隔離App/CLI，但未啟動、未覆寫其他build或凍結App。

fake rate的deadline由transport建構時計算、累積payload bytes，閒置會累積credit，後續可突發零等待；因此420kB/s是長期平均模型，不是每次payload固定成本。fake write忽略timeout參數，這次不驗timeout語意。區域due/missed等是該區最後一列的觀測值，不能保證包含其後未再次被選中的變化。frame/done v0僅處理wall busy，scheduler實際busy另加capture_ms，不能把表內sum當完整scheduler成本。右區靜止，無送出年齡不是陳舊內容年齡；所有完成/傳送指標只是fake主機端，不代表LCD。

建置/前置試跑記錄不全：最終commands.json僅保留正式targeted compile與兩run，首次缺platform.o的link錯誤原始輸出及部分前置命令未留，不宣稱完整失敗軌跡。原始不同run日誌仍在logs，正式case請以analysis.json的精確log_directory對照。

最終唯讀核對：兩run stream/end t_us=5016449/5014990，log/end=5026237/5024291；lost_full=0、lost_after_failure=0、events write_failed=0。no-send間隔以stream/end為端點。左右中間隔包含初始off/full基準；左分區14次另加初始full一次，右只有初始full，所以右尾端是右設限，非已完成的相鄰更新間隔。continuous左hash相鄰299/299不同；週期左相鄰300/300不同，間隔42來源slot的259/259相同。

原JSON payload_bytes_diagnostics誤名已更正為wire_bytes_diagnostics=2108052（含114×18-byte window commands）；fake payload實際2106000，兩者都不含握手/回覆/USB overhead。左區最大host-send間隔含尾端為533.897/533.904ms，中央69.648/69.647ms，右尾端4921.424/4921.430ms。
