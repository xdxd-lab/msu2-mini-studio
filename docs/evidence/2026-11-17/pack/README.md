# 2026-11-17 精選證據包（去識別化副本）

公開版本：本目錄是經 QA 審核後公開的去識別化精選副本（第 4 版），由原工作機器 `results/macos/` 的原件複製並替換識別資訊；原件未修改，完整原件與凍結 App 仍保存在原機器，不公開也不刪除。現行結論以[現況與待辦](../../../STATUS_zh-TW.md)為準，目錄說明見[證據索引](../../../EVIDENCE_INDEX_zh-TW.md)。

## 一、來源與版本
- 產品基線：commit `e9c5f584`，tree `dd6b63c1…`（凍結候選的建置來源）。這是測試當時的產品基線，不代表本文件合併後的 `main` head。
- 凍結候選：GUI `MSU2MiniStudio` SHA-256 `abe86bc1…19e8ae`；CLI `msu2ctl` SHA-256 `82d8f4b8…1b2fd`（已對原件重算核對，與 02 direct 命令紀錄一致）。
- 02 direct 兩輪：2026-11-17；01a：2026-11-15；06 attempt_004：2026-11-16（離線單次執行，非實機）。

## 二、內容（22 個資料檔，白名單；另含本說明、MANIFEST.csv、SCAN.txt）
| 目錄 | 內容 |
|---|---|
| `02_direct/raw30`、`02_direct/raw60` | command、exit_status、preflight、stdout、stderr、start_epoch、events.log、diagnostics.csv、stream_1.csv |
| `02_direct/` | QA1 更正附記 |
| `06_offline/` | attempt_004 報告與更正附記（DEV 撰寫，依 QA1 審核） |
| `01a_legacy_raw24/` | 01a 報告（Raw24 短送，2026-11-15）；文首置頂標示其「共用鎖」敘述不成立 |

未納入：02 foreground 審核（無 MSU2 數據）、05 離線判準與 SHA 索引、06 SHA 索引（指向未附的 binary 與 dSYM）。

## 三、判讀邊界
- **01a 不是合格的共用鎖試驗**：報告內文原敘述已由同檔更正推翻，已於文首置頂標示。
- **02 direct 未控制桌面內容**：Raw／full／off 每幀固定 39018 B；diagnostics 與 CSV 只證明主機送幀、ACK 與寫入一致，不證明 LCD 更新、逐像素公平性或內容新鮮度。
- **正式窗原點**：以 session／meta 的 `stream_begin` 時間為準（raw30 t_us 212342、raw60 t_us 234611），不是 log 起點（t_us 0）。
- **送幀率**：raw30 378 幀／16 s＝23.6 fps；raw60 383 幀／16 s＝23.9 fps。兩者皆未達 30／60 目標。stdout 的 `host_fps=0` 不採用。
- **欄位不同義**：events.log 的 `waiting_frames_replaced` 與 stdout 的 `dropped` 在本輪數值相同（141／793），但名稱與計數層級不同，不應視為同義。
- **preflight**：為先前結果，非執行當下量測。
- **06**：離線、單次執行，不得視為實機結果或無洩漏證明。

## 四、去識別化規則（逐檔，次數見 MANIFEST.csv 的 rules 欄）
- 專案根目錄與家目錄路徑 → 占位符 `<PROJECT_ROOT>`、`<HOME>`。
- 裝置埠序號 → `usbmodem<REDACTED>`；程序編號 → `<PID>`；電郵、session 連結與識別碼、團隊角色代號、測試代理名稱、其他應用程式與外接顯示器名稱 → 占位符。
- `diagnostics.csv` 的 `rx_hex` 非空值 → `<REDACTED_RX_HEX>`；`rx_len` 保留。該檔以 csv 模組重寫，位元組與原件不同。
- 人工文字修改（`E_manual_edit`）：01a 置頂更正與原檔連結改為 SHA 說明；06 標題改為 DEV；02 附記的送幀率算式與正式窗原點說明。
- 其餘數值、時間戳、幀統計與命令參數不變。不含像素、token、session ID。

## 五、可重算性邊界
- 每檔有**原件 SHA256 與 bytes**與**輸出 SHA256 與 bytes**；未替換的檔案輸出 SHA 與原件相同。
- 替換過的檔案不能只靠輸出 SHA 還原原件，需以原件 SHA 核對後重算；規則為確定性，理論上可由原件重算。
- 去識別化腳本未附（內含原件絕對路徑），需由 QA1 在原件所在機器執行並留存腳本 SHA。
- `MANIFEST.csv` 的 src 欄已套用程序編號占位符。

## 六、檔案
- `MANIFEST.csv`：逐檔 dest、src、原件與輸出 SHA、bytes、替換規則、狀態。
- `SCAN.txt`：殘留敏感掃描（pattern ID、命中檔案與判讀）。
