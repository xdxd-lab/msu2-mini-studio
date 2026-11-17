# 2026-11-12～11-15 歷史報告精選（去識別化副本）

公開版本：本目錄是經 QA 審核後公開的去識別化歷史報告精選（第 4 版），由原工作機器 `results/macos/` 的原件複製並替換識別資訊；原件未修改，完整原件仍保存在原機器，不公開也不刪除。現行結論以[現況與待辦](../../../STATUS_zh-TW.md)為準，同日證據包見[精選證據包](../pack/README.md)。

## 一、內容（10 份，人工挑選）
| 目錄 | 內容 | 原件來源（相對名） |
|---|---|---|
| `02/` | 2026-11-13 驗證報告 | `msu2_02_verify_20261113/` |
| `05/` | 2026-11-12 controller（fake）報告；2026-11-12 ACK A/B 報告；2026-11-13 paired 分析；2026-11-14 獨立審閱；2026-11-15 static lowload attempt_002 報告 | `msu2_05_*` |
| `06/` | 2026-11-14 resource attempt_003 報告與審閱 | `msu2_06_resource_20261114/attempt_003/` |
| `07/` | 2026-11-12 disconnect gate 報告；2026-11-14 stream gate 審閱 | `msu2_07_*` |

**未納入**：08 preview.2 報告（已過時，發布結果以 STATUS 為準）；06 attempt_004 兩份與其審閱（已在同日[精選證據包](../pack/README.md)的 `06_offline/`，不重複）；12 離線判準審閱（含已棄用的前景守門器內容）；02 11-12 報告（引用未附的產物，且基準 commit 已過時）；所有 SHA 索引、原始 CSV 與 events。

## 二、被取代與過時狀態（閱讀前必讀）
- **06 attempt_003（2026-11-14，de590ff 基準）已被 attempt_004 取代**。attempt_004 在 e9c5f584 上重跑並附更正，結果見同日精選證據包的 `06_offline/`。本包的 attempt_003 只保留作為 ASan 前身紀錄。
- **07 報告第 15 行「02 最小設計…尚未實作」已過時**：截至 2026-11-12 的狀態。依 PM 交接記錄，metadata 診斷已於 PR #4 合併（commit e9c5f584）。現況以[現況與待辦](../../../STATUS_zh-TW.md)為準。
- **05 審閱（2026-11-14）第 4 節列出的「待做」退化案例**：其中靜止情境已由 2026-11-15 attempt_002 做過（見本包 05 的 static lowload 報告）。其餘案例仍為待做，未驗。
- 所有結論皆為當時口徑（fake、離線或單次執行），不是實機最終結論。**現行結論以 `docs/STATUS_zh-TW.md` 為準**；與 STATUS 衝突時，以 STATUS 為準。

## 三、正文引用但未公開的產物
各報告正文提到的 logs、JSON、patch、腳本與原件目錄**均未公開**，包括但不限於：ACK0_GAP_CHECK、analysis 腳本與 analyze.py、各類 *.json、*.patch、run_stream_gate.sh、source/ 目錄，以及各輪的 diagnostics.csv、stream CSV。
**本包只是結論與判讀留存，數字不能由本包重算。** 需要重算請向原件持有者或 QA1 索取。

## 四、窗口與時間原點
05 報告的正式窗（+3 s 至 +19 s，16 秒）以 **session／meta 的 stream_begin 時間為原點**，不是 log 檔的起點（t_us 0）。

## 五、去識別化與替換統計
- 10 份中 **6 份有替換**（02 verify、05 REVIEW、05 static lowload、06 attempt_003 報告與審閱、07 REPORT），**4 份原樣未改**（05 controller fake、05 ACK A/B、05 paired 分析、07 stream gate 審閱）。原樣未改者，輸出 SHA 與原件相同。
- 替換規則：
  - 專案根目錄與家目錄路徑 → `<PROJECT_ROOT>`、`<HOME>`。
  - 裝置埠序號 → `usbmodem<REDACTED>`；程序編號 → `<PID>`。
  - 電郵、session 連結與識別碼 → 占位符。
  - 團隊角色代號與內部任務編號 → 移除，改為角色名稱（DEV、QA1 等）。
  - 測試代理名稱與模型等級 → 改為「執行者」「審閱者」。
  - 其他應用程式與外接顯示器名稱 → 占位符。
- 數值、時間戳、命令參數與結論不變。

## 六、可重算性邊界
- 每檔有**原件 SHA256 與 bytes**與**輸出 SHA256 與 bytes**。
- 替換過的 6 檔不能只靠輸出 SHA 還原原件，需以原件 SHA 核對後重算；規則為確定性，理論上可由原件重算。
- 去識別化腳本未附（內含原件絕對路徑），需由 QA1 在原件所在機器執行並留存腳本 SHA。
- 本包不能從 Git 重算報告內的數字（見第三節）。

## 七、檔案
- `MANIFEST.csv`：逐檔 dest、src、原件與輸出 SHA、bytes、替換規則、狀態。
- `SCAN.txt`：殘留敏感掃描（pattern ID、命中檔案與判讀）。
