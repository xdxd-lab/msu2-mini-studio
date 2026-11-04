# 桌面串流診斷紀錄

2026-11-05 原始碼整理：背景 writer 已拆至 `msu2_macos_app/src/diagnostic_log.hpp/.cpp`；Session 事件欄位在 `msu2_linux_v0_3/include/msu2/trace.hpp`。下文 App 路徑與「仍待驗證」描述是 10/30 建立時的狀態；最新實機結果與限制見 `handoff/RESUME_AUDIT_20261105_zh-TW.md`，原始證據仍在本機 `results/`。

此功能預設關閉。`MSU2_DIAGNOSTICS=1` 記錄摘要；設為 `2` 另記每個 payload 寫入呼叫的時間和長度，可能增加負荷。紀錄保存在該次 Controller 日誌資料夾的 `diagnostics.csv`，與 `events.log`、`stream_N.csv` 一起判讀。

最近實機使用的本機診斷候選是 `results/macos/stream_trace_20261030/hardware_candidate_rxclass/MSU2 Mini Studio.app`，已完成 10/31 與 11/05 桌面串流。架構整理後的原始碼須另行建置，不能把舊候選視為新建置。App 使用臨時簽章，授權情況可能不同於既有版本；請使用確切路徑，避免選到同名舊版。Git 不包含這些本機候選。

紀錄包含設定、幀與 patch 編號、視窗座標、長度、處理時間、佇列取代、握手／ACK／寫入階段、例外與重連。只保存既有規則識別到的握手或 ACK pattern；其他收到的位元組只記數量，不保存原始內容。桌面像素、Pair 顏色與 payload 不記錄。ACK 前綴不代表完整回覆或 LCD 已完成顯示。

時間來自單調時鐘，檔頭的牆鐘錨點僅有秒精度。`captured` 是 `Source::get` 返回時間，並非 ScreenCaptureKit 影格的 PTS，因此佇列年齡不能當成畫面實際年齡。診斷日誌由背景執行緒寫入；緩衝滿會計數並提示。正常釋放 Controller 後才寫入 `log,end`：未見結束列、看到遺失或寫入失敗時，應視為不完整；異常終止可能遺失最後事件。

`rxclass` 依既有 read 資料統計 banner、MSNCN 及 `02 03` 次數，包含跨讀取的樣式；分類不影響 ACK 接受規則。`rxread` 最多記錄每次等待的前 32 筆非空讀取時間與長度，超限另計數；單次讀取即成功時省略這些重複資訊。兩種事件都不保存未知原始 bytes，完整欄位契約見 `trace.hpp`。

失敗階段及診斷開關已經 fake transport 測試，完整離線套件與 ASan／UBSan 通過。寫檔故障與緩衝溢位的安全注入尚未驗證。實機已重現 Pair24／28 的 window ACK timeout，尚未定位根因；詳見 `handoff/RESUME_AUDIT_20261105_zh-TW.md`。修改前的使用者實機紀錄摘要見本機 `results/macos/stream_trace_20261030/prior_log_review_zh-TW.md`。
