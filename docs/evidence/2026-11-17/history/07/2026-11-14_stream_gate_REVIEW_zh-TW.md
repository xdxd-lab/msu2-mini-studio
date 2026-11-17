# Stream disconnect gate 證據審閱

結論：現有測試碼及兩份執行紀錄支持所設計的離線情境通過。這是 fake transport 與 fake lease 的測試，不能推論實機串流、實際序列埠或其他時序均安全。

測試先連線，將第一個 stream payload 的 `write_all` 阻擋在 gate；確認進入且當時 payload_writes 為 1 後呼叫 disconnect。gate 尚未放行時斷言 opens=1、leases=1 與 busy=true；放行後等待 disconnected、busy=false、lease=0，並檢查第一個 transport 恰好 close 一次。其後重新連線並等待握手完成，再 disconnect，檢查第二個 transport 也 close 一次、lease 最終歸零。這個「後繼握手」是握手完成後斷線，不是握手進行中斷線。

`run_stream_gate.sh` 對 normal 與 ASan/UBSan 各建置並各呼叫 watchdog 一次；watchdog 使用 30 秒 subprocess timeout。兩份 run JSON 均記錄 exit 0，stderr 為空，前後 binary SHA-256 記錄各自一致；輸出均為 opens=2、closes=2、leases=0。紀錄呈現每模式一次結果，不足以獨立排除該腳本先前曾被執行或紀錄覆寫。ASan/UBSan 編譯旗標見腳本，並非從 binary 獨立驗證。

兩次輸出最終 payload_writes 都是 550：程式只在 gate 初次進入時檢查為 1，不能宣稱 disconnect 後沒有進一步 payload write，也不能由此證明立即中止寫入。watchdog 傳入的 runtime 路徑參數未由此測試的 `main()` 使用，測試使用固定的 `/tmp/msu2-stream-disconnect-gate`。本審閱僅讀取現有證據，未重跑測試或修改產品。
