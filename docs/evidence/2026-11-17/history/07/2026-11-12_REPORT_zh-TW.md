# MSU2-07 第一批離線結論

基準 202bac688df5f3439acbc97611ef28599f0569eb。隔離來源 source/，原 main 與既有 PR checkout 不動。來源逐檔與 git archive 比對見 source_verification.json；額外加入 fake 測試與 targeted script，未修改產品。

main 與 PR #1 d18abda008011c3a93814e3210fffee0af67ee6f 的 controller.cpp blob 均為 312996fb0668ea15c5d6adec943e589f14ef5d33，沒有 Controller 衝突。

測試由 執行者 執行；時序推論由 審閱者 審閱。targeted build exit0、20秒外層 watchdog run exit0。commands/exit/runtime/binary SHA 在 disconnect_gate_probe/。未跑完整套件或 sanitizer；本輪沒有產品修改。

fake transport destructor gate 停住 close：重複 disconnect 之後 connect 沒有進入新連線；舊 lease 保持到 gate 釋放。等待斷線完成事件後再 connect，第二個握手 write gate 期間 disconnect，最後 opens=2、leases=0、connected=false、busy=false。events=2 是 fake write呼叫數（兩次握手），不是 diagnostics 事件數。

靜態推論：旗標 true 期間 submit 在 mutex 下拒絕新工作；close 中再次 disconnect 可合併為已在完成的舊連線關閉。clear=false 後才可接受後繼 connect；worker單執行，後繼的新 disconnect 沒有該次舊 clear 可再次覆蓋。未找到目前可達的後繼連線請求遺失，故不修產品，也沒有 red/green 修復宣稱。

限制：gate覆蓋的是close內、後繼握手內兩種交錯，不是指令邊界的全交錯證明；close尚未返回時snapshot仍connected且busy=false，本測試只記現況，不定義這是理想UI語意。connect在關閉中是拒絕而非延後，若使用者預期排隊重連需另定需求。下一步可由QA獨立重跑此最小probe；若有不同可見錯誤，需提供API呼叫次序與預期語意再擴充，暫不猜改鎖。

02 最小設計見 DESIGN_02_zh-TW.md：來源PTS、epoch/seq、快取重讀與現有queue-age proxy分開；尚未實作，時鐘映射需官方SDK契約確認。不記錄像素，且主機寫完不代表LCD完成。
