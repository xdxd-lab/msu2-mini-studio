# attempt_004 報告更正附記（DEV，依 QA1 審核；原始證據與 REPORT 不改）

1. lost_after_failure 對調更正：write fault（mode1）＝16625；flush fault（mode2）＝16612（依 run/evidence/mode1、mode2 的 events.log「診斷紀錄結束」行）。REPORT 表中兩值寫反。
2. footprint 單位與數值更正：REPORT 的「22→155→282→326 MB」實為 MiB（2^20），且取的是 teardown 值。以 probe stdout 原值（bytes，十進位 MB）為準：
   - 各段 peak：172.97 / 304.88 / 352.69 MB；teardown：162.19 / 295.80 / 341.50 MB；process_start 約 23 MB。
   - 第 3 段增量：peak 304.88→352.69（+47.8 MB），teardown 295.80→341.50（+45.7 MB）；REPORT 的「+44 MB」更正為約 +45 MB（teardown，MB）。
3. 結論不變：ASan 下 footprint 不能作洩漏判讀；**06 沒有洩漏或無洩漏結論**。threads 1→4→1、FD 4→6→4 回 baseline 的觀察不受影響。
