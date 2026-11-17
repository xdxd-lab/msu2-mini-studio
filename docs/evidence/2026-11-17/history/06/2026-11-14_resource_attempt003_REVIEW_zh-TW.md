# attempt_003 只讀審閱

此回離線 fixture 執行紀錄顯示 PID <PID> 於 120.924 秒以 exit 0 結束，150 秒 watchdog 未觸發，runner 記錄的 binary 執行前後 SHA-256 相同，回收後 signal-zero 得 ProcessLookupError。stdout 三段各約 40 秒，sent_sample → sent_after 分別 3 → 1206、7 → 1210、7 → 1210；各段 handshake failure、ACK timeout、transport error、reconnect 均為 0。這些是測試 fixture 的觀察，不是硬體串流結果。

lsof 有 25 次樣本，runner 的 `fd_count` 為 6 兩次、8 二十三次，未見樣本間持續上升；該值是 lsof 輸出列數，包含 cwd 與 txt，並非純數字 FD 數。快照中分別為數字 FD 0–2 或 0–4；取樣有間隔，不能排除其間的變化。ps 首次遭 PermissionError 後停用，RSS、CPU、thread 數均未知；sampler_errors=0 不代表 ps 成功，因錯誤被記為 unknown。

fake lease 與 fake transport 是物件生命週期計數，三段 teardown 後均為 0；source_callback_inflight 是當時執行中的 callback 計數，不是 source 物件數。程式另檢查 teardown 後 20 ms 內 source 與 ostream 呼叫數不再增加，不能推論所有產品資源或長時間行為。

正常模式有 log,end，events 記錄 write_failed=0；注入 write 與 flush 失敗兩模式各記錄一次警告、串流繼續、write_failed=1 和 lost_after_failure，並無 log,end，因此診斷資料不完整。此處使用測試用 ostream adapter，不等同實際檔案系統故障驗證。不能由此宣稱無洩漏或長時間 soak 通過；先前 attempt 的失敗與證據限制仍保留。本審閱未重跑測試或修改產品。
