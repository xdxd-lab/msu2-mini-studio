MSU2 Mini Linux C++ v0.3.1 候選版
2026-10-11

這份原始碼從封存的 C++ v0.2 複製後獨立開發。v0.2 的協定、局部差異規劃、
序列埠傳輸與基本測試保留；新加的 priority.cpp 與 stream --priority-mode
實作分區降載。未改寫裝置韌體，也不執行 Flash 寫入。
SOURCE_BASE.json 保存從 v0.2 複製前的 19 項檔案 SHA-256，LICENSE 保留原授權。
原始碼封存檔不含本機測試產生的 Mach-O 執行檔；在 Linux 請重新建置。

建置（Linux，需 C++17 與 pthread）：
  bash scripts/build.sh
  ./build/msu2 --help
  ./build/msu2 ports

選擇其中一種分區模式，例如：
  ./build/msu2 stream --port /dev/ttyACM0 --pattern moving --mode pair --update diff --keyframe-ms 0 --chunk 60 --fps 45 --frames 450 --priority-mode bands4 --csv bands.csv
  ./build/msu2 stream --port /dev/ttyACM0 --pattern noise --mode pair --update diff --keyframe-ms 0 --chunk 60 --fps 45 --frames 450 --priority-mode center3 --csv center.csv
  ./build/msu2 stream --port /dev/ttyACM0 --pattern moving --mode pair --update diff --keyframe-ms 0 --chunk 60 --fps 45 --frames 450 --priority-mode halves2 --csv halves.csv
  ./build/msu2 stream --port /dev/ttyACM0 --pattern moving --mode pair --update diff --keyframe-ms 0 --chunk 60 --fps 45 --frames 450 --priority-mode off --csv off.csv

off 為預設值，保留 v0.2 的原串流排程、局部更新與 CSV 欄位。
send 可用 --hold-open-ms 0..60000（預設 13）在 payload 寫入完成後、關閉
序列埠之前等待。期間不再傳輸或讀取資料，亦不是 LCD 完成回覆；Ctrl+C 可
提前結束等待。stdout 記錄要求與實際等待毫秒數，供關埠時序對照。
13 ms 是依使用者後續觀察選定的暫用預設，實際等待受排程影響，仍待持續觀察。
明確指定 --hold-open-ms 0 可做立即關埠的對照；stream 不加入此等待。

v0.3 將預設 --chunk 從 3840 調為 60，因 3840 在既有 macOS 實機測試中曾使第二個開窗
ACK 逾時。Linux 裝置與傳輸效率尚待原生驗證，這項預設不是 Linux 效能結論。

三個模式共同先送首張全幅，累積一次成功傳輸的主機耗時，再對每張來源畫面
計算原本 full/diff 方案的預估主機成本。預估成本達影格週期 85%、前次實際
忙碌達 95%，或等候中的來源畫面被取代，會在下一張來源畫面進入選定的分區。
切換只發生於成功或略過當前更新之後，不中斷正在寫入的封包。

  bands4   160x20，上→下四條輪替；不主動略過排定的橫帶。
  center3  中央 80x80→左 40x80→右 40x80；中央時槽維持原頻率，
           側區若預估耗時達週期 80% 或前次實際忙碌達 90%，可略過該側區。
           若只有預估偏高、上一時槽實際忙碌不超過 70%，且側區距上次
           主機同步已滿 1000 ms，允許一次探測傳輸，避免長期沒有新樣本。
  halves2  左 80x80→右 80x80；不主動略過排定的半幅。

分區相位根據來源 frame_id 推進。即使 LatestFrame 等候格被新影格覆蓋，
原先排定的區域時槽仍計入該區 missed_queue，而非假裝未安排。分區取當下
最新來源畫面的區域；與主機最後成功送出的 160x80 畫布比較，完全相同便
列為 unchanged。區域發送成功才更新畫布該區；失敗時立即使參考畫面失效，
程序停止。下一次啟動需要重新握手並先送全幅。沒有自動重新連線。

回到原 full/diff 排程需要連續至少 max(fps,20) 個已處理影格的安全預估，
且在分區停留至少 1000 ms：原方案預估低於週期 60%，前次實際忙碌低於
70%，沒有新的等候格取代；若該時槽定期全幅已到期，該全幅的預估也要低於
60%。預估值來自成功傳輸之開窗時間與 payload bytes
耗時的 EWMA，並加上當前擷取、轉換與編碼耗時。首次傳輸前沒有預估；
閾值是待 Linux 實測校準的初始設定，非設備規格。

若 --update diff 的週期性 keyframe 已到期，當前分區時槽直接送一張全幅，
但不因此強制退出分區模式；成功後全幅參考畫面、所有區域的同步來源編號及
last_full 一起更新。該時槽仍只計入原排定區域的 due/sent。一般負載回復
的判斷以未強制 keyframe 的 full/diff 預估為主，但到期全幅若很昂貴也會
阻止安全時槽累積。若 keyframe 傳輸失敗，
不更新參考畫面並停止。本功能會造成定期全幅成本；可用 --keyframe-ms 0
關閉週期性 keyframe，以便單獨評估分區演算法。

CSV 的前 15 欄保留舊定義。啟用分區時附加 requested/active 模式、區域、
切換時間、分區持續時間與觸發原因、預估時間、來源編號缺口、區域 due/sent/unchanged/skipped/
missed_queue、左右各區連續漏失與最久不同步時間。intentional skipped、
來源 queue drop、unchanged、傳輸失敗分別計數；以「每區已排定時槽」為
漏更新分母。CSV 只記錄完成的來源時槽；傳輸失敗會立即中止並在 stderr
輸出 adaptive_failure，含 frame、區域、ACK 逾時分類、階段與已完成的
封包位元組數，不會把未完成的封包當成成功列寫入 CSV。max_stale_ms 是截至
已觀察時槽、距主機最後成功同步的最大間隔，不代表 LCD 實際延遲。
host_update_fps 只代表主機完成的區域或全幅更新，不能視為 LCD 的全幅刷新率。

目前較確定的傳輸問題與邊界：
- macOS 上靜止且停止 TX 3 秒的兩個 45 FPS 對照正常；6 秒的 45/60 FPS
  都在恢復首個開窗 ACK 逾時且讀到 MSN01。兩個隔離實驗在原等待逾時後
  以同一 fd 嚴格重握手與全幀恢復成功；尚不能認定裝置物理休眠或精確門檻。
- MSN 首次出現約在開窗後 11–16 ms，原等待仍持續約 2 秒。提前辨識／
  重新握手尚未納入本候選版，因此三種排程不處理會話失效。
- 3840-byte 區塊的第二開窗問題與上述閒置 MSN 路徑不同；連續移動中的
  偶發逾時也需分別排查。macOS 45..60 FPS 初掃沒有呈現明確失效 FPS 門檻。
- prefix ACK 只驗證舊協定的 02 03 前綴，OS write_all 只代表主機提交；
  分區統計與成功退出不等於 LCD 實際完成或沒有殘影。

來源測試規則參考同工作區 docs/STATIC_TRANSITION_ASTRA_ANALYSIS_20261011_zh-TW.txt
及另一聊天 01a12629-3ee2-7f13-94b2-12133b6828cf 的最新討論。
後續可在 bands4 能撐住時再比較上 160x20→中 160x40→下 160x20 三橫帶；
目前 --priority-mode 僅接受 off、bands4、center3、halves2 四個值。

需在 Linux 原生環境與實際 MSU2 上驗證建立裝置、階段性負載、ACK、
來源取代、各區漏更新與畫面品質。macOS 的模擬序列埠測試不能替代此驗證。
