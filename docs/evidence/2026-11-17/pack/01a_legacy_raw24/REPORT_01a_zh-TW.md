# 01a GUI 實機短檢查

> **置頂更正**：下文「序列埠與操作」所述「成功取得共用 flock、持有至串流停止」**不成立**。GUI 與外層使用的鎖根不同，本輪不是合格的共用鎖試驗。詳見文末「更正（2026-11-15）」。

日期：2026-11-15（Asia/Taipei）

## 範圍與候選

- 限於 frozen 候選 App 的桌面預覽、一次 GUI 握手，以及一次短 Raw desktop 串流；未執行 Pair、05、Flash 或長測。
- App：`results/macos/msu2_02_candidate_20261115/frozen/MSU2 Mini Studio.app`，PID <PID>；GUI 工具回報的完整 App 路徑與指定路徑相同。
- GUI SHA-256：`abe86bc11c2dd830317f749f31922f1b623a54a0cc29b87bf9d693c5da19e8ae`；CLI SHA-256：`82d8f4b8fb423d798f936510d2e6041b765441806575777ab4bd2b6aa9a1b2fd`；App CDHash：`9b69b6f7e26a9028f287f33ef489f954bbb60d92b8d62a209b57605c8f721add`。測試前後重新核對兩個 binary 的 SHA，均與指定值相符。
- 預覽：按「更新預覽」後 160×80 畫面顯示實際桌面內容。

## 序列埠與操作

- 裝置：`/dev/cu.usbmodem<REDACTED>` 與 `/dev/tty.usbmodem<REDACTED>` 均存在。測前 `/usr/sbin/lsof -nP -- <cu> <tty>` 無擁有者輸出、exit 1；`serial_guard.check_idle` 同時檢查兩別名並回報 lsof exit 1。以 `SerialGuard(..., lock_root="results/macos/.device-locks")` 成功取得共用 flock，持有至串流停止、GUI 中斷連線後才釋放。
- GUI 握手：2026-11-15 22:43:59 成功，MSN banner=1、韌體識別 1。App 明示 banner 回覆不是面板完成回執。
- 串流：22:45:31–22:45:49（18 秒），desktop、Raw、full、priority off、24 FPS、Hold 1 ms、ACK timeout 3000 ms；自動重新連線關閉。22:45:49 停止並寫入 CSV；22:46:00 GUI 中斷連線並釋放序列埠。
- GUI 最終統計：440 傳送、0 未變更、0 ACK timeout、0 握手失敗、0 傳輸錯誤、1 reconnect、2 等待影格取代、0 來源丟幀；ACK 1.1 ms、寫入 54.9 ms。LCD 實際更新率未量測。事件記錄中的閒置會話重新驗證成功，並記錄 `waiting_frames_replaced=2`。
- CSV 有 440 筆，全部 outcome=`sent`，wire_bytes 每筆 39018；`dropped_total` 最終值 2。此短檢查只確認 GUI/串流路徑可工作，不代表 LCD 更新率或更長時間穩定性。

## 證據

- 原檔未公開；以下 SHA-256 僅供原機器核對：events.log `61dbf81f5219726be5bcb2f3bfd0e39e9db11df302d379f0afd2873416c2b6f7`，stream_1.csv `41db85a8fdcb175831725eac4533aa811e49a81b9bc4da2d6f8cf660153e674a`。
- 原始 App 日誌位於執行者本機，路徑未公開。

## 更正（2026-11-15）

後續確認 PID <PID> 沒有 `MSU2_SHARED_LOCK_ROOT` 環境變數，因此 GUI 使用隔離 clone 的鎖根；先前由外層 Python 取得的是主專案鎖根。兩者不是同一把共用鎖，故本報告上文「共用 serial guard 持有至 disconnect」不成立。原 CSV/events 保持未修改，不能把該輪視為滿足共用鎖要求的合格硬體試驗。

另外，UI 的 `reconnect=1` 應解讀為閒置會話重新驗證（events 記錄 idle renewal 成功），不是串流故障後自動重連。`waiting_frames_replaced=2` 是被新影格取代的等待影格；`source missed=0` 是另一個獨立計數。CSV 的 440 筆是已送出資料列；觀察到的 frame ID 範圍為 0–441，不能由此推斷 LCD 顯示或更新率。18 秒為 events 起訖時間的粗略窗長，440/18 僅是粗略主機送出比率，非 LCD FPS。

## 重新啟動門檻檢查（2026-11-15）

- 舊 PID <PID> 已退出。`cu`／`tty` lsof 無持有者；主專案與隔離 clone 的 lock file inode 分別為 118365、3256909，檢查時兩者 flock 均可取得。
- 以指定 frozen App 路徑及 `open -n --env MSU2_SHARED_LOCK_ROOT=<主專案>/results/macos/.device-locks --env MSU2_DIAGNOSTICS=1` 啟動新 PID <PID>。唯讀 process environment 讀回未能解析出 `MSU2_SHARED_LOCK_ROOT` 或 `MSU2_DIAGNOSTICS=1`，所以未按預覽、未連線、未開始 02；也沒有從外層另取 Python 鎖。
- 已退出新 PID <PID>。隨後重新檢查 cu／tty lsof 仍空，兩鎖 inode flock 均未持有。此輪未產生新的硬體串流 CSV/events；02 尚未執行。

## PID <PID> 環境讀回證據檢討（唯讀，2026-11-15）

- 當時執行的讀回命令為 `/bin/ps eww -p <PID> -o command= | /usr/bin/python3 -c '<inline parser>'`。Python parser 搜尋字串 `MSU2_SHARED_LOCK_ROOT=`，再搜尋其後的字面標記 ` MSU2_DIAGNOSTICS=`，以兩個標記之間的整段文字比對預期完整鎖根；並確認 diagnostics 標記後的文字以 `1` 開頭。它沒有用空白切分鎖根值。
- 當時工具僅回傳 parser 輸出：`PID <PID> shared-lock env verified: False`、`value: MISSING_OR_UNPARSEABLE`、`diagnostics=1: False`。`ps eww` 的原始 stdout、stderr 及其 exit status 沒有被保留，因此目前無法回溯判斷輸出是空、變數缺失、被截斷，或格式不同。
- 因 parser 是以 `MSU2_DIAGNOSTICS` 作為終止標記而非按空白分詞，主專案路徑含空白本身不足以解釋此次 `False`。不過因原始 stdout 未保存，無法核實 `ps` 實際輸出格式，也無法排除中間層對環境文字做了其他格式化；因此先前「變數未驗證」仍是正確結論，「環境變數確定缺失」則證據不足。
- 本次只檢閱已保存的命令記錄與結果，未重新執行 `ps`，未啟動 App，未存取 USB。
