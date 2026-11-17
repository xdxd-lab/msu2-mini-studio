# MSU2-05 獨立驗收審閱（2026-11-14，只讀既有證據；無產品修改/USB/SCK/推送）
## 1 前後比較：實際只有兩組配對
- 配對1 ACK2000 左變右靜（baseline vs candidate，msu2_05_paired_20261113）、配對2 ACK0 雙側變（同）：我從 diagnostics.csv/stream_1.csv 獨立重算正式窗 [stream_begin+3s,+19s)，與 analysis/REPORT 逐項一致：
  ACK2000：中央 sent 169→170（10.5625→10.625/s），中央 gap max 107.9→112.5ms；左 sent 0→15、skipped 148→128；右 unchanged 151→135 且 sent 0；wire 3,298,542→3,464,580 的組成（中央 3,298,542→3,318,060；左 0→146,520）。
  ACK0 雙側：中央 173→175（10.8125→10.9375/s），gap max 153.5→201.2ms；左 0→15；右 15→15；左 gap max 1102.97ms，右 1147.7→1107.6ms；左/右各 146,520 B。
  每次側區 15 次 sent、gap 約 1.07–1.11 s，與「持續 dirty 1 秒即升級」設計一致。
- 【已更正，見 ACK0_GAP_CHECK_zh-TW.md】ACK0 左變右靜的 baseline+candidate 在 msu2_05_side_bound_20261112/verification/runs_diag，我先前說「無 candidate」錯誤。
## 2 與已合 main 的等價性
- 8f3676b 對 7d6d287 的產品差異恰為 3 檔：priority.hpp、priority.cpp、controller.cpp（+45/−3）；其 +/- 行與 PR submitted.patch 逐行比對，main 側無額外行（0），patch 較多的 36 行屬測試/文件。2228f6c→8f3676b 的 msu2_linux_v0_3/src、include、msu2_macos_app/src 無差異（PR3 之後未再動產品源碼）。
- 限制：等價性以「git 內容」核對；本次未重新計算凍結候選的 234 項 SHA（沿用 PR 的 equivalence.txt，未獨立重驗），也未重編二進位。probe 使用的凍結 baseline/candidate 二進位是否與 8f3676b 編出的位元相同——未證實。
## 3 分母與單次限制
- 每案單次執行、無重複；差值（如中央 +0.0625/s、gap +4.6ms）在單次排程變異內，不可當效能差異。中央 gap max 在雙側案增 +47.7ms，是單次最大值，不是分佈。
- 分母：中央 sent/s=sent/16；整筆 frame 按 done 時刻歸窗，跨邊界 wire 不切分；queue replaced 取樣計數邊界可能落後；fake source/transport，payload 速率 420000 B/s 為人工設定，非實機或 SCK。
- fingerprint 判斷右側「有變」可能碰撞；無逐次 dirty trace，不能量測 dirty→dispatch 延遲，15 次 sent 只證明升級有發生。
- 全程 stream 週期 8 秒類相位別名風險同 MSU2-05 前報告：人工來源公式（5 秒週期）可能影響 unchanged 判定。
## 4 有資訊增益、可離線做的退化案例（皆「待做」，非已驗）
1 靜止/低負載 center3：兩側皆靜止或中央輕量，驗 side_dirty 不誤升級、wire bytes 與 baseline 相同（回歸檢查，最高價值）。
2 30FPS target：負載較低，驗側區仍由正常 slot 服務、升級不重複送（確認不增加 wire）。
3 恢復後 full：pause/resume、失敗後 planner.invalidate 清 committed，驗 side_dirty 在 committed 為空時不觸發（controller 條件 committed.size()==current.size()），以及恢復後第一個 full 之後行為。
4 側區變動中途停止/重新出現：dirty 1 秒計時是否重置、oldest-first 在兩側同時到期的順序（已有單元測試，可補 Controller 層）。
5 ACK 大延遲（如 20ms）與高 payload 成本：升級是否讓中央 gap 超過可接受上限（量測分佈，不只 max），重複 N≥5 次給變異範圍。
6 單側 ACK0 candidate（補齊缺的配對）。
建議 DEV 最小選擇：案 1（靜止）＋案 6（補配對）＋重複次數；成本低且直接降低回歸與單次不確定。
