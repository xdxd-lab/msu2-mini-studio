# MSU2-05：禁止 idle credit 的 Controller ACK A/B

## 結論與範圍

fb3cfc3aa9494af4e6cdb49bb22b97a67a4405e4 的234項產品來源/符號連結前後一致。兩組均正常完成、完整正式窗，正式16秒內左區持續變動但零send；這是人工來源/傳輸下的現行Controller+Scheduler現象，不是實機證據或Q4確定性重播。未修改產品、ACK規則、timeout或排程，沒有SCK/USB/App啟動/推送。只執行A/B兩組。

## 固定條件

source設定desktop但Dependencies.source提供假影像；fake lease/transport。center3/60、Raw/full、chunk60、hold1ms、timeout3000ms、auto_reconnect=false、trace1。中央與左每來源frame變動，右靜止，沿用5秒probe公式（8-bit公式會循環，不是無限不重複）。來源每次轉RGB565後記自訂區域fingerprint，未存影像或raw RX。

payload nominal420000 bytes/s，每write_call deadline=max(previous_deadline, now)+ceil(bytes/rate)；閒置及chunk間不累積credit。A window ACK不注入延遲，B注入2000µs；握手不注入延遲。各3秒暖機+16秒正式，按session/meta stream_begin的[+3000,+19000)ms及frame done裁窗。first source後19秒才停止，原始stream/end確認覆蓋正式右邊界A+1.769ms/B+41.874ms。

## 正式窗結果

| 案例 | 區域 | sent | skipped | unchanged | sampled due / missed |
|---|---|---:|---:|---:|---|
| A 0ms | 中央 | 184 | 0 | 0 | 320 / 136 |
| A 0ms | 左 | 0 | 132 | 1 | 320 / 187 |
| A 0ms | 右 | 0 | 0 | 136 | 320 / 184 |
| B 2ms | 中央 | 182 | 0 | 0 | 319 / 137 |
| B 2ms | 左 | 0 | 131 | 1 | 319 / 187 |
| B 2ms | 右 | 0 | 0 | 138 | 322 / 184 |

sent/skipped/unchanged按正式窗完成列計數；due/missed是每區最後觀測計數差，端點不是全域同步，不能視為精確960槽分配。missed為scheduler來源ID缺口分配，不直接等同queue取代。

正式窗各有960個capture，左/中央相鄰fingerprint皆959/959變動，右0/959。capture missed=0/0；正式窗queue replacement計數差507/506，全段605/606。左區從初始full host send到正式窗終點已18877.415/18876.149ms沒有新send（含暖機，右設限下界），正式窗完整16秒零send。中央最大相鄰host-send間隔含邊界108.945/115.125ms。右也無後續send，但來源靜止且一直unchanged，不得稱其飢餓。左有一次unchanged可能為循環內容回到參考，不能當新傳送。

## 實際注入與完整性

整段window ACK requested0/2000µs，observed median3/2508µs、p95=5/2551µs。60-byte payload chunk底層計費約142.858µs（CSV requested_us整數顯142）；observed median194/190µs、p95=229/226µs，write budget中位2967ms。每call無credit會包含主機sleep overshoot，實際速率不等於nominal。完整injection.csv保留每call請求/實耗/bytes/剩餘budget。

payload總量A4270500、B4290000 bytes；不可與包含window command的wire bytes混用。兩組handshake_failures/ack_timeouts/transport_errors皆0、stop_reason=duration；stream/end、log/end完整，lost_full=0/lost_after_failure=0，events最終write_failed=0。全部編譯/link/run exit0，run process elapsed A19.461s/B19.086s；無watchdog timeout。若有fault本runner會停，不分類成飢餓。

## 證據與限制

- run_20261112/commands：每命令argv/exit/stdout/stderr/time/timeout；meta.txt列BUILD_DIR與命令。
- 實際BUILD_DIR為source/msu2_macos_app/build_ack_ab_20261112，只產probe及物件，非正式build、未產新App。
- run_20261112/binary_sha256.txt、input_sha256.csv；執行檔前後相同。
- A_ack0、B_ack2000：meta、source_hashes、injection、Controller events/diagnostics/stream CSV。
- analysis/analyze.py、A_ack0.json、B_ack2000.json：可重算統計；BASE.json保存來源manifest。

無穩定clock hook，真steady_clock與sleep有抖動；每種只一次，未量到每次skip的完整prediction。此A/B能顯示兩種ACK設定下同樣有持續左側未送出，不能證明小延遲的因果、一般ACK門檻或實體LCD行為。相較先前5秒probe，時間窗、idlecredit模型與編譯選項等有差，不能將結果差異只歸因單一改動。沒有第三組、全套或ASan延伸。
