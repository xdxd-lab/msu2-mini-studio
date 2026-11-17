# MSU2-05 配對 probe 分析

四案均使用隔離的 fake source 與 fake transport，各執行一次 3 秒暖機、16 秒正式窗。分析以 diagnostics `stream_begin` +3 至 +19 秒為正式窗，按 frame done 時刻歸屬傳送。四案皆自然結束，stream end 超過正式窗，握手失敗、ACK timeout、transport error 均為 0；均有診斷紀錄及 log end，最終 `lost_full=0`、`lost_after_failure=0`、`write_failed=0`。四案 JSON 和 `analyze.py` 可由 `../verification/cases/<case>/probe_output` 原始資料重算。

## 左變、右靜：ACK 2 ms

| 正式窗指標 | baseline | candidate | 差值（candidate − baseline） |
|---|---:|---:|---:|
| 中央 sent / 秒 | 10.5625 | 10.6250 | +0.0625 |
| 中央相鄰 host send 最大間隔 ms | 107.881 | 112.524 | +4.643 |
| 左 sent / skipped / unchanged | 0 / 148 / 1 | 15 / 128 / 0 | +15 / −20 / −1 |
| 右 sent / skipped / unchanged | 0 / 0 / 151 | 0 / 0 / 135 | 0 / 0 / −16 |
| 左相鄰 host send 最大間隔 ms | 無兩次 send | 1,109.287 | 不適用 |
| queue replaced / capture missed | 493 / 0 | 512 / 0 | +19 / 0 |
| 總 wire bytes | 3,298,542 | 3,464,580 | +166,038 |
| 中 / 左 / 右 wire bytes | 3,298,542 / 0 / 0 | 3,318,060 / 146,520 / 0 | +19,518 / +146,520 / 0 |
| 總 payload bytes | 3,295,500 | 3,461,250 | +165,750 |
| 中 / 左 / 右 payload bytes | 3,295,500 / 0 / 0 | 3,315,000 / 146,250 / 0 | +19,500 / +146,250 / 0 |
| 左 / 右 override | 0 / 0 | 15 / 0 | +15 / 0 |

baseline 左側由正式窗前最後一次涵蓋全畫面的 host send 至窗末未再 send，觀察間隔 18,877.696 ms，窗末右設限；靜止右側兩案均零 send。

## 雙側變：ACK 0 ms

| 正式窗指標 | baseline | candidate | 差值（candidate − baseline） |
|---|---:|---:|---:|
| 中央 sent / 秒 | 10.8125 | 10.9375 | +0.1250 |
| 中央相鄰 host send 最大間隔 ms | 153.507 | 201.249 | +47.742 |
| 左 sent / skipped / unchanged | 0 / 137 / 1 | 15 / 111 / 0 | +15 / −26 / −1 |
| 右 sent / skipped / unchanged | 15 / 132 / 0 | 15 / 130 / 0 | 0 / −2 / 0 |
| 左相鄰 host send 最大間隔 ms | 無兩次 send | 1,102.969 | 不適用 |
| 右相鄰 host send 最大間隔 ms | 1,147.720 | 1,107.627 | −40.093 |
| queue replaced / capture missed | 500 / 0 | 514 / 0 | +14 / 0 |
| 總 wire bytes | 3,523,134 | 3,708,690 | +185,556 |
| 中 / 左 / 右 wire bytes | 3,376,614 / 0 / 146,520 | 3,415,650 / 146,520 / 146,520 | +39,036 / +146,520 / 0 |
| 總 payload bytes | 3,519,750 | 3,705,000 | +185,250 |
| 中 / 左 / 右 payload bytes | 3,373,500 / 0 / 146,250 | 3,412,500 / 146,250 / 146,250 | +39,000 / +146,250 / 0 |
| 左 / 右 override | 0 / 0 | 15 / 15 | +15 / +15 |

baseline 左側從正式窗前最後一次涵蓋全畫面的 send 至窗末未再 send，觀察間隔 18,880.185 ms，窗末右設限。兩個雙側 case 的來源右側 fingerprint 均有 256 種，相鄰 1,140 對均不同；兩側 case 的 tag 與 fingerprint 均支持右側變動的設定。fingerprint 可能碰撞，不能證明逐次精確 dirty 狀態。

## Window ACK 注入觀察

下表單位 µs，為 `injection.csv` 的整次執行範圍，非正式窗。requested 為注入設定；observed 為 probe 從 window 發送至讀取 ACK 的實際經過時間，包含排程影響。p95 使用排序後第 `floor((n−1)×0.95)` 筆。

| case | 次數 | requested median / p95 / max | observed median / p95 / max |
|---|---:|---:|---:|
| ACK2000 left baseline | 200 | 2000 / 2000 / 2000 | 2520 / 2560 / 2598 |
| ACK2000 left candidate | 220 | 2000 / 2000 / 2000 | 2526.5 / 2560 / 2693 |
| ACK0 both baseline | 225 | 0 / 0 / 0 | 3 / 5 / 40 |
| ACK0 both candidate | 247 | 0 / 0 / 0 | 3 / 5 / 140 |

每組只有一次 baseline 和一次 candidate，排程與量測變異可能影響差值。傳送間隔並非連續 dirty 等待；本 probe 無逐次精確 dirty trace，無法由此證明每個 dirty 到期事件或成功期限。相鄰間隔僅計正式窗內兩次成功 host send；無兩次時不填。wire 和 payload 依 frame done 整筆歸屬，跨邊界傳送未切分；queue/capture 值是 capture event 採樣的計數差，邊界可能落後。host send / ACK 不證明 LCD 完成。override 時 region due 並非純名義槽；此表使用實際 outcome。沒有執行實機測試。
