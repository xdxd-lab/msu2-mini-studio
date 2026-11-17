# MSU2-05  靜止情境補做 — attempt_002（2026-11-15，DEV）

依計畫決定執行。attempt_001 的檔案（含靜止 baseline 失敗證據）未更動。

## 結論

- 握手 fixture 自測：baseline、candidate 各一次，皆 exit 0、全部檢查通過。
- 靜止 baseline、靜止 candidate 各一次，皆有效完成（exit 0、stop_reason=duration、正常 log,end、故障計數全 0）。
- 兩案行為相同：正式窗內**沒有任何側區或中央的分區傳送**（override 0），但各有 **3 次整幀重送**（每次 39,018 B，合計 117,054 B）。原因是產品在 Raw full 模式下，畫面完全靜止仍會每約 5.2 秒閒置重握手一次，握手後下一張強制整幀。所以「靜止正式窗不應傳送」這個預期在本設定下**不成立**，而且 baseline 與 candidate 都一樣，不是 candidate 引入的。
- 重握手間隔取決於 fake 的 banner 時點（5114 ms，取自歷史實機紀錄的單一數值），不是本次量到的實機行為；實機是否如此未驗證。

## 執行數與預算

 原訂最多四案。實際執行：attempt_001 三次（靜止 baseline 失敗 1、低負載 2），attempt_002 兩次（靜止 baseline、candidate），**共 5 次案例執行，超出原預算 1 次**；另有 2 次握手自測（非串流案例）。超出原因：attempt_001 的靜止 baseline 因 fake transport 不會在閒置時送 MSN banner 而中止，該次不產生靜止結果，經 PM 決定 A 授權擴充 fake 後重做靜止配對。沒有任何案例在同一 attempt 內重跑。

## 設定

離線 fake lease／fake transport／fake source；未碰 USB、SCK、GUI、App、遠端，未改產品。60FPS、中央與兩側全程不變（center_period=0）、420000 B/s、no idle credit、ACK 0、Raw full（differential=false）、center3、chunk 60；3 秒暖機＋16 秒正式，正式窗＝[stream_begin+3s, +19s)，依 frame done 歸窗。

- 來源與前置核對同 attempt_001（`preflight.json` 全部通過）。
- Fixture 變更（`fixture/FIXTURE_DIFF_vs_attempt_001_probe.patch`）：Fake 與 Shared 移到 `fixture/fake_transport.hpp` 供 probe 與自測共用；Fake 新增閒置 banner——距最後一次 TX 返回滿 5114 ms 且接收佇列為空時，送出一次 `00 4D 53 4E 30 31`，下一次 TX 後重新計時。probe 其餘邏輯不變，meta 多一列 `fake_idle_banner_after_ms=5114`。
- 5114 ms 的依據：來源樹 `docs/history/SESSION_STATE_ASTRA_ANALYSIS_20261011_zh-TW.txt` 記錄實機 first_banner_ms≈5114（自最後一次 TX 返回起算）。「每個閒置期只送一次」沿用產品自帶測試 `msu2_macos_app/tests/test_controller_edges.cpp` 的 fake 寫法；實機在該紀錄中約 350 ms 內出現 4 個 banner，**後續重複的 banner 未模擬**。
- 建置：`build.py` 重編 12 個產品 object 並連結 probe 與自測。新 object 雜湊與 attempt_001、舊凍結 object 皆不同（原因未查），所以 attempt_002 的 binary 與 attempt_001 不是同一批 object。
- Binary SHA256（執行前後相同，`HASHES_BEFORE.txt`／`HASHES_AFTER.txt`）：probe baseline `f7250d84…091b2253`、candidate `03f85566…3e1e6137`；自測 baseline `ea3a615c…8fad2f21`、candidate `7fea74d1…d5784ed2`。
- Runner：`run_selftest.py`、`run_one.py`（拒絕覆寫、先記 PID/argv/start、finally 回收 child、記錄 exit 與前後雜湊）；完整命令在 `COMMANDS.txt`。

## 握手自測（`self_test/<role>/`）

以未修改的產品 `msu2::Session::handshake()` 驅動 fake：

| 檢查 | baseline | candidate |
|---|---|---|
| 初次握手回傳版本 1 | 通過 | 通過 |
| 最後一次 TX 後 5000 ms 內無任何位元組 | 通過 | 通過 |
| 閒置期只出現一次 `00 MSN01`，共 6 bytes | 通過 | 通過 |
| banner 出現時點（ms，自最後 TX） | 5114.78 | 5114.46 |
| MSNCN 逐位元組 echo | 通過 | 通過 |
| 閒置 4520 ms 後呼叫產品握手成功，完成於 ≥5114 ms | 通過（5114.53） | 通過（5114.99） |
| exit／秒數／PID | 0／11.00／77840 | 0／11.02／77934 |

與真協定的對照：banner 格式（`00 M S N` 加兩位數字）與 MSNCN echo 符合產品握手程式的接受條件與歷史實機紀錄；時點採單一歷史數值；重複 banner、banner 與可接受命令之間的競態（歷史分析列為候選問題）都沒有模擬。我判定假回覆與已知協定無矛盾、但只涵蓋其中一部分，因此繼續執行並在此標明範圍。

## 靜止案結果（`cases/`、`analysis/`）

| 項目 | static60_baseline | static60_candidate |
|---|---|---|
| exit／秒數／PID | 0／19.34／77999 | 0／19.36／78085 |
| 有效、stop_reason | 是、duration | 是、duration |
| handshake failure／ACK timeout／transport error | 0／0／0 | 0／0／0 |
| log,end（written／lost_full／lost_after_failure） | 4220／0／0 | 4223／0／0 |
| 閒置重握手（整次執行） | 3 次，皆成功，各約 0.60 秒 | 3 次，皆成功，各約 0.60 秒 |
| 整幀送出時點（秒，自 stream_begin） | 0.128、5.370、10.607、15.853 | 0.124、5.369、10.618、15.859 |
| 正式窗整幀 sent | 3 | 3 |
| 正式窗 wire bytes | 117,054（全為整幀） | 117,054（全為整幀） |
| 正式窗分區 sent（中央／左／右） | 0／0／0 | 0／0／0 |
| 正式窗分區 unchanged（中央／左／右） | 278／275／276 | 278／276／276 |
| side_dirty_deadline override | 0 | 0 |
| 整幀相鄰間隔最大值 | 5246.2 ms | 5248.1 ms |
| 正式窗 queue replaced／capture missed | 128／0 | 127／0 |
| 整次執行 payload_bytes、queue_replaced | 156,000、134 | 156,000、133 |

queue replaced 集中在每次約 0.6 秒的握手等待與約 0.13 秒的整幀傳送期間（60FPS 來源持續產生幀而傳送端被占用），兩案相當。

## 解讀與限制

- 可以說：在本 fake 條件下，candidate 於完全靜止畫面沒有誤送側區、沒有觸發 side_dirty_deadline，與 baseline 的可觀測行為一致。
- 不能說：靜止時不傳送。兩案都會週期性整幀重送，這是產品既有的閒置重新驗證行為（controller.cpp 的 4500 ms 門檻；Raw full 下 planner 對靜止畫面仍回傳非空 plan）。是否要讓靜止畫面免於週期性重握手，是產品決策，本批未改也未建議修改。
- 各單次執行，fake 傳輸完成不等於 LCD 顯示完成；實機的 banner 時點、重複次數與重握手成功率未驗證。
- 分析使用未修改的 `msu2_05_paired_20261113/analysis/analyze.py`；重握手與整幀時點另以 diagnostics.csv 直接讀出。
