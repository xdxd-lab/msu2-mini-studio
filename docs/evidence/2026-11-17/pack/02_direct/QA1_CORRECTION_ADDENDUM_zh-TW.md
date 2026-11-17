# MSU2-02-DIRECT 更正附記（QA1，2026-11-17，唯讀）

本附記不覆寫原始證據與先前的 QA 報告，只更正判讀。未重跑、未開埠、未操作桌面、未碰 USB。

## 一、更正
1. **鎖檔判讀更正**：SerialGuard 設計會保留鎖檔與最後的 PID 文字，因此「鎖檔存在且內容為 pid <PID>」不能作為「鎖仍被持有」或「未釋放」的證據。
   - raw60 PID <PID> 已退出，當下 `lsof` 無 owner。
   - 當時的 flock 狀態沒有留下紀錄，只能說「原始當時 flock 狀態證據不足」。
   - 先前報告寫的「鎖未釋放」與「raw30 鎖 pid <PID> 已被覆寫」，都超出原始證據，應撤回。raw30 執行時鎖檔內容未留存。
2. **桌面像素與送幀速率判讀更正**：Raw／full／off 每幀固定 39018 B。未控制桌面像素，不影響 HOST 送幀速率觀察的有效性，因此 [+3,+19) 的 bytes 與送幀率可作為主機端觀察採用：raw30 為 378 幀／16 s＝23.6 fps；raw60 為 383 幀／16 s＝23.9 fps（三種計算口徑——半開區間、閉區間、以首幀為基準——結果相同）。正式窗原點為 session/meta 的 stream_begin 時間（raw30 t_us 212342、raw60 t_us 234611），不是 log 起點（t_us 0）；[+3,+19) 皆以此為基準計算。
   - 但本次不能做逐像素公平性、內容新鮮度或 LCD 更新結論。
   - 先前報告中「送幀數未反映 FPS 參數、主機端節流為瓶頸」屬於推論，需另以主機端 timing 證據驗證，不能當作已證實的結論。
   - stdout 的 `host_fps=0` 仍與 CSV 計算值不一致，且原因未查明，不採用。

## 二、preflight 與 SHA 再審
- `raw30/preflight.txt` 與 `raw60/preflight.txt` 內容相同（diff 無差異）。
- 兩檔均記載 msu2ctl `82d8f4b8…1b2fd` 與 MSU2MiniStudio `abe86bc1…19e8ae`，與 expected 相符（match=True）。
- `CGPreflightScreenCaptureAccess=true` 標註為「prior system-context result」，是執行前的先前結果，不是執行當下量到的值。TCC 因此不能作為執行當下的證據。
- 執行前 `lsof`、`pgrep` 結果皆為空（exit 1）；執行後當下的 lsof 也無 owner。兩者都不是執行中的證據。

## 三、日誌一致性
| 項目 | raw30 | raw60 |
|---|---|---|
| diagnostics 資料列（含 log 2 列） | 7185 | 9245 |
| 其中 log begin／end | 2 | 2 |
| 事件 `written` 計數 | 7183 | 9243 |
| 差額 = begin／end | 2 ✓ | 2 ✓ |
| CSV 資料列（sent） | 519 | 527 |
| diagnostics frame done | 519 | 527 |
| tx window_write／rx window_ack／payload write | 519／519／519 | 527／527／527 |
| stdout sent／dropped／bytes | 519／141／20250342 | 527／793／20562486 |
| CSV dropped_total 最終值 | 141 | 793 |
| exit 結束時間計算 | 22.306 s ✓ | 22.296 s ✓ |

- 兩輪 `written` 差額 2 即為 log begin／end 兩列，不計入計數器。數量一致。
- stdout 的 sent／dropped／bytes 與 CSV 及 diagnostics 相符，只有 `host_fps=0` 不一致。
- 兩輪 handshake_failures、ack_timeouts、transport_errors、write_failed 皆為 0。

## 四、結論
- 更正後的判讀為：兩輪 exit 0，主機端送幀與 ACK 一一對應，日誌計數一致。
- 主機端 [+3,+19) 送幀率約 23.6（30 目標）與 23.9（60 目標），bytes 可由幀數乘 39018 核對。
- 不能宣稱 LCD 更新率、逐像素公平性、內容新鮮度，也不能宣稱 TCC 於執行當下為 true。
- 鎖檔殘留不是「未釋放」的證據，原始當時 flock 狀態證據不足，需要另行決定是否補查設計或記錄。
