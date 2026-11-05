# MSU2 Mini Studio 軟體架構

本文說明原生 macOS App 的結構和執行期規則。結果與限制以[現況](STATUS_zh-TW.md)為準；接手流程見[交接入口](HANDOFF_zh-TW.md)。

## 模組

| 位置 | 內容 |
|---|---|
| `msu2_linux_v0_3/` | 共用核心，不依賴 macOS UI |
| `msu2_macos_app/src/controller.cpp` | 工作排程、連線、串流、素材 Flash 與圖片讀回流程 |
| `msu2_macos_app/src/diagnostic_log.*` | 選用的診斷紀錄，內部模組，不對外公開 |
| `msu2_macos_app/src/flash.cpp` | 外部素材 Flash 的讀取、擦寫計畫與驗證 |
| `msu2_macos_app/src/serial_guard.cpp` | 序列埠的 cu/tty 擁有者檢查與共用鎖 |
| `msu2_macos_app/src/platform.mm` | 螢幕擷取、裝置列舉、圖片載入與儲存 |
| `msu2_macos_app/src/app.mm`、`ui_views.*`、`preview.cpp` | App 介面與預覽 |
| `msu2_macos_app/src/cli.cpp` | `msu2ctl` 命令列 |

核心裡的主要元件：

- `protocol`：命令與視窗編碼。
- `session`：握手、ACK 等待與 payload 寫入，同一個 Session 只能由建立它的執行緒使用。
- `transport`：序列埠 I/O。
- `delta`：差分規劃，`DeltaPlanner` 保存參考畫面。
- `priority`：分區排程。
- `latest.hpp`：只保留最新一張的單格佇列 `LatestFrame`。
- `trace.hpp`：診斷事件和 `TraceSink` 介面。

MCU 韌體燒錄沒有實作。「Flash」在本專案一律指外部素材 Flash。

共用核心的 `main.cpp` 是獨立 CLI，`capture.cpp` 保留早期 Windows 擷取實作；兩者都不編入 macOS App。App 的桌面來源由 `platform.mm` 提供。

## 執行緒與擁有權

| 執行緒 | 負責 | 可以碰的資料 |
|---|---|---|
| API 呼叫端（UI、CLI） | 提交工作、設定旗標、複製 `Snapshot` | 持有 `mutex` 才能動 `state`、`pending` |
| worker | 執行 `loop()` 和所有工作，也是唯一做裝置 I/O 的執行緒 | `session`、`transport`、`lease`、`connection`、`identity`、`last_activity` 等 |
| producer | 只存在於 `stream()` 期間，取得來源畫面 | `Source`、`LatestFrame`、擷取計數器 |
| 診斷 writer | 只在診斷開啟時存在，格式化並寫檔 | `DiagnosticLog` 內部緩衝 |

- `mutex` 保護 `state`、`pending` 和 `events`。`log_directory` 在 worker 啟動前就固定，之後只讀。
- `wake` 的條件所讀的旗標必須持有 `mutex` 才能修改，由 `set_flags` 處理，否則可能漏掉喚醒。
- 釋放順序是 `session`、`transport`、`lease`。`diag` 比 `session` 活得久，因為 Session 持有它的裸指標。

## 連線狀態

`Snapshot.state` 的轉換：

```text
disconnected -> connecting -> ready
ready -> streaming <-> paused
ready -> flashing
ready -> downloading
任何狀態 -> disconnected（close）
```

- 工作結束後，`loop()` 依 `connected` 收斂到 `ready` 或 `disconnected`。
- 工作拋出例外時狀態是 `error`，保留到之後的工作改寫它。
- 閒置重新驗證時短暫顯示 `connecting`，成功後還原先前的狀態。

閒置超過 4500 ms 後，下一次傳送前會重新驗證同一連線：先等 MSN banner，再確認 MSNCN。這是應用層的保守門檻，不是量測到的韌體常數。

## 串流資料流

```text
Source -> LatestFrame -> to_rgb565 -> DeltaPlanner -> PriorityScheduler -> Session::send
```

- producer 依目標 FPS 取畫面。工作趕不上時，等待中的畫面被新畫面取代，不會累積。
- 正在傳送的畫面不會被中斷。

### 參考畫面的提交規則

`committed` 和 planner 的參考畫面保存「最後一張完整交付的畫面」。

- 一次更新的所有 patch 都從 `send()` 返回後才提交。這只證明收到 ACK 前綴且主機寫入完成，**不代表 LCD 已顯示完成**。
- 規劃本身不提交；未變更和被略過的時段也不提交。
- 分區更新只把該矩形併入 `committed`。
- 傳輸失敗、暫停後恢復、閒置重新驗證都會清空兩者。面板內容此時未知，所以下一次會傳完整畫面，而且不分區。

### 時間定義

計時和間隔都用 `steady_clock`。牆鐘只用於日誌檔名、`events.log` 和 `log,begin`，精度到秒，不能用來計算間隔。

| 名稱 | 定義 |
|---|---|
| `captured` | `Source::get` 返回的時間，不是 ScreenCaptureKit 的 PTS 或到達時間 |
| `capture_ms` | `Source::get` 的耗時 |
| `before` | 這一幀開始處理的時間；閒置重新驗證的耗時會被扣除 |
| `window_ack_ms` | 從寫入 window 之前到收到 ACK，**包含 window 寫入** |
| `payload_write_ms` | ACK 之後到 payload 全部寫入 |
| `encode_ms` | 處理總時間減去 ACK 和 payload 寫入後的剩餘 |
| `resume_time` | 恢復的時間；更早擷取的畫面視為過期並丟棄 |
| `last_activity` | 最後一次完成的裝置交換，用於判斷閒置 |

桌面閒置時 `grab()` 可能重複回傳快取的同一張圖，所以佇列年齡會低估畫面的實際年齡。

CSV 的 `dropped_total` 是 frame id 缺號數，不是佇列取代次數。佇列取代次數在 `Snapshot.queue_dropped`。

## 診斷紀錄

- 預設關閉。`MSU2_DIAGNOSTICS=1` 記錄摘要，`=2` 另記每次 payload 寫入。
- 輸出是該次日誌目錄的 `diagnostics.csv`。完整欄位見 `diagnostic_log.hpp` 和 `msu2/trace.hpp`。
- `record()` 只寫入預先配置的緩衝，背景 writer 每 200 ms 寫一次。緩衝滿或寫檔失敗時只計數並提示，串流不會因此停止。
- 這不代表零效能擾動：`record()` 仍要取鎖和複製事件，writer 也會占用 CPU 與磁碟，等級 2 的事件量更高，影響未量測。比較 FPS 時要對照診斷關閉的結果，至少用等級 1，不要用等級 2。
- 啟動失敗時視同關閉診斷，App 照常執行。
- `log,end` 只在 Controller 正常解構時寫入。**沒有 `log,end` 的檔案一律視為不完整**，所以要等 App 結束後才能取最終檔。
- 不保存桌面像素、Pair 顏色、payload 或未知的 RX 原始 bytes。RX 只留已識別樣式的計數和 read 時間。

可用 `open --env` 對新實例明確指定診斷環境。以下路徑對應 README 的隔離建置；執行前先結束先前的實驗，避免兩份 App 同時操作裝置：

```sh
open -n --env MSU2_DIAGNOSTICS=1 -a "$PWD/results/local-build/MSU2 Mini Studio.app"
```

以該實例的 `events.log` 診斷啟用訊息及 `diagnostics.csv` 的 `log,begin` 核實，不從啟動命令推定記錄已生效。詳細判讀見[診斷](DIAGNOSTICS_zh-TW.md)。

## 建置與測試

```bash
msu2_macos_app/scripts/build.sh
msu2_macos_app/scripts/test.sh
BUILD_DIR=/path/to/isolated SANITIZERS=1 msu2_macos_app/scripts/test.sh
BUILD_DIR=/path/to/build msu2_macos_app/scripts/build_hardware_driver.sh
```

- `build.sh` 預設輸出到 `msu2_macos_app/build`，會改寫既有固定路徑的 App，ad-hoc 簽章下可能要重新授權螢幕擷取。驗證候選版時請設定 `BUILD_DIR`。
- `launch_current.sh` 只開啟預設路徑的 App，不會開啟 `BUILD_DIR` 的建置結果。
- `build_hardware_driver.sh` 只編譯，要先用相同的 `BUILD_DIR` 跑過 `build.sh`。
- 新增 `.cpp` 時，要同時更新 `build.sh`、`test.sh` 的來源清單；如果 `controller.o` 依賴它，也要更新 `build_hardware_driver.sh`。

## App 身分與螢幕授權

bundle identifier 目前是 `local.msu2mini.studio`。固定路徑、隔離建置及凍結候選可能同名；核對實際程序與 App 路徑，不透過名稱搜尋選版本。

ad-hoc 重建可能使原授權失效。依系統提示授權後，結束並從確切路徑重新啟動；CLI 與 App 的擷取授權須分別確認。不要自動重設 TCC 或將可顯示預覽等同於取得裝置握手。`MSU2_CODESIGN_IDENTITY` 只選簽章身分，正式簽章與升版驗收見[發布準備](RELEASING_zh-TW.md)。

## 修改邊界

- 不改傳輸協定、ACK 接受規則、timeout 或顯示更新演算法，除非另有明確決定。
- 實機測試同時只能有一個序列埠擁有者。操作前要檢查 cu/tty 兩個別名和共用鎖，並凍結執行檔、保留 SHA256。
