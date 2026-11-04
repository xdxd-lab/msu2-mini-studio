MSU2_MINI C++ v0.2.0 — 跨幀差異更新原型
日期：2026-10-10

本版新增逐像素比較與局部傳送，沿用 MINI 160×80 的既有開窗／RGB565 頁協議。
使用者已確認局部更新是可行方向；本版新實作的實物速度與顯示正確性仍待回傳測試。
Windows x64 執行檔在 bin/windows-x64/msu2.exe。測試與建置結果見 docs/TEST_REPORT_zh-TW.txt。

一、最先使用的指令

在解壓後、看得到 bin 的資料夾開啟 PowerShell。先確認實際 COM 埠：
  .\bin\windows-x64\msu2.exe ports

先測 30 FPS 差異更新：
  .\bin\windows-x64\msu2.exe stream --port COM3 --pattern moving --update diff --keyframe-ms 0 --fps 30 --frames 300 --csv diff30.csv 2>&1 | Tee-Object -FilePath diff30.log

接著測 60 FPS 目標：
  .\bin\windows-x64\msu2.exe stream --port COM3 --pattern moving --update diff --keyframe-ms 0 --fps 60 --frames 600 --csv diff60.csv 2>&1 | Tee-Object -FilePath diff60.log

上述 moving 測試持續有畫面變化，使用 --keyframe-ms 0 排除週期全幅刷新的影響；第一張仍完整傳送。
COM3 必須換成實際裝置埠。若你已在 bin/windows-x64 資料夾，執行檔路徑改成 .\msu2.exe。
每次測試請用不同 CSV 與 log 名稱。CSV 已存在時程式會拒絕開始，保留舊資料。
CSV 會在握手成功後才建立，正常每約一秒刷新到檔案。
本版沒有增加 Flash 或 MCU 韌體寫入功能。

二、兩個互相獨立的模式

--update full|diff：跨幀更新策略，僅適用 stream，預設 full。
  full：每個取出的來源畫面都傳送整張，供與 v0.1 的方式比較。
  diff：與最後一張完整送出的 RGB565 畫面比較，只傳需要更新的區域。

--mode raw|pair：區域內的頁編碼策略，適用 send、stream、encode，預設 pair。
  raw：每頁完整寫入。
  pair：每頁預填常見的四位元組組合，再寫不同的位置。

因此 --update diff --mode pair 會同時使用跨幀差異更新與既有頁內壓縮。

三、差異更新如何保持正確

1. 連線後第一張一定送完整畫面，建立參考影像。
2. 先轉為 RGB565，再逐像素比較；RGB888 微小變化若量化後相同，不重送。
3. 評估整張、差異包圍矩形、合併 8×8 區塊及精確水平差異段。
4. 以實際編碼位元組加每個區域 384 位元組的固定成本估計，選擇較低成本方案。
   384 是選擇啟發值，用來考慮開窗回應等待，不會額外送出這些 bytes，也不是實測時間。
   合併矩形可能包含少量未變像素，這能减少零碎開窗與尾頁填補成本。最多 32 個區域。
5. 當前更新的全部區域送完後，才提交新的參考影像。傳送失敗就終止，不能把未完成影像當基準。
6. 來源產生較快時，只保留最新等待畫面；比較仍使用最後完整送出的影像，不會漏掉被略過幀的變化。
7. 完全相同的畫面不送資料，計入 unchanged_frames，並非 host_frames_sent。

--keyframe-ms 1000：diff 模式預設約每秒在下一個處理畫面時完整重送一次。
這是保守的刷新策略，可減少裝置待機或狀態不同步的影響，完整重送時延遲會較長。
值可設 0～60000；0 表示停用週期刷新，第一張仍完整傳送。此選項要求 --update diff。
它不是經實物驗證的裝置心跳保證。確認自己的韌體在靜態畫面下正常後，可用 --keyframe-ms 0 比較差異更新的連續表現。

多個區域依序傳送，不代表 LCD 能原子切換整張。仍應觀察殘影、撕裂與邊界位置。
Ctrl+C 會在目前更新完成或發生 I/O 錯誤後結束。每次區域傳送沿用 I/O 期限，跨區域也會檢查整次更新的時間；已在進行中的區域仍按自己的 I/O 期限完成或失敗。

四、CSV 與主控台

前十個 CSV 欄位沿用 v0.1：
  frame_id：來源幀編號；缺號通常是等待幀被更新的來源取代。
  capture_ms：擷取／產生來源時間。
  convert_ms：轉為 RGB565 的時間。
  encode_ms：本版包含差異比較、區域規劃與候選編碼時間。
  window_ack_ms：當次所有區域的開窗及等待回應時間總和。
  payload_write_ms：當次所有區域資料寫入時間總和。
  host_latency_ms：來源產生完畢至主機完成當次處理，包含等待時間，不含 capture_ms。
  wire_bytes：本次實際送出的應用層命令位元組。
  write_calls：本次傳送函式分塊呼叫次數。
  dropped_total：等待幀被取代的累積數量。

新增欄位：
  changed_pixels：相對上一張完整送出影像的變動像素數。首次建立參考值為 12800。
  transmitted_pixels：選取區域所覆蓋的像素數，可能包含合併後的未變像素。
  rectangles：此次更新的區域數。
  update_kind：full / bounding_box / tiles / runs / unchanged。
  unchanged_total：完全相同且未傳送畫面的累積次數。

CSV 每一列是完成處理的來源畫面，因此包括 unchanged 列；這些列 wire_bytes、write_calls、rectangles 均為 0。
主控台 host_frames_sent 只計算有送出區域的來源畫面，processed 包含 unchanged。
正常跑完時 captured = host_frames_sent + unchanged_frames + waiting_frames_replaced。
手動中斷或發生錯誤時可能還有尚未處理的等待畫面，不能硬套這個等式。
full_frames、partial_frames、rectangles_sent 可用來判讀全幅刷新與局部更新的分布。
OS 寫入完成與 LCD 真正完成顯示不同；host_send_fps 不代表面板 FPS。

五、離線測量

CPU 轉色／頁編碼基準，介面與 v0.1 相同：
  .\bin\windows-x64\msu2.exe bench --frames 2000 --csv cpu_bench.csv

新的跨幀資料量比較：
  .\bin\windows-x64\msu2.exe diff-bench --frames 300 --csv diff_bench.csv

每一組測 --frames 次；diff-bench 範圍 1～100000，預設 300，測六種圖樣與 raw/pair。
比較同一固定序列逐張全幅傳送與差異更新的總 bytes。首張完整，後續不加週期全幅刷新。
輸入生成與 RGB565 轉換不在 prepare_ms 內；計時含比較與候選編碼，未暖機，先 full 後 diff，僅供初步觀察。
沒有 USB、LCD、擷取節拍或等待幀取代，不可把資料量節省直接當成實物 FPS 倍數。

六、其他指令與限制

  .\bin\windows-x64\msu2.exe --help
  .\bin\windows-x64\msu2.exe probe --port COM3
  .\bin\windows-x64\msu2.exe send --port COM3 --pattern bars
  .\bin\windows-x64\msu2.exe preview --pattern bars --output bars.ppm
  .\bin\windows-x64\msu2.exe encode --pattern bars --mode pair --output bars.commands.bin
  .\bin\windows-x64\msu2.exe stream --port COM3 --desktop --update diff --fps 30 --frames 300 --csv desktop_diff.csv

來源三選一：--pattern bars|solid|checker|gradient|noise|moving，或 --input picture.ppm，或 --desktop。
--desktop 僅 Windows（主要螢幕居中 2:1 裁切）。encode 不接受 --desktop。
PPM 支援 P3/P6、maxval1～255，縮放／裁切成160×80；目前不直接讀 PNG/JPEG/GIF。
encode 的 .bin 是離線 LCD 命令串流，不是 MCU 韌體或 Flash 素材檔。

傳輸選項保留：--timeout-ms 2000（1～60000）、--chunk 3840（6～65532，6的倍數）、--ack prefix|echo、--orientation 0|1。
prefix 只檢查既有 02 03 回覆前綴，不表示面板完成。echo 是實驗診斷模式；方向角度映射尚未驗證。
每個 send/stream 自行握手，不需先 probe；其他上位機需關閉。若裝置未重新廣播，等待待機或重新插拔。

七、建置

Linux/macOS：bash scripts/build.sh
Windows MSVC：powershell -File scripts/build_windows.ps1
macOS 可使用圖樣／PPM與串列核心；桌面擷取尚未實作，macOS 建置與執行仍待本機驗證。
本包 bin/linux-x64/msu2 是 Linux 執行檔，不能當成 Mac 執行檔。

八、目前的實物基準

v0.1 使用者 Windows MINI 實測：兩種插孔皆完成 15 FPS 色條；30 FPS moving 皆產生 300 張、送出 236 張、取代 64 張。
USB 2.0 主機送出 23.5016 FPS，USB 3.0 為 23.4637 FPS。
USB 3.0 moving 平均 payload_write_ms = 41.959009；轉色與編碼合計約0.125ms。
USB 2.0 moving 的逐幀 CSV 缺失，僅有完整總紀錄；使用者已決定不補，不以推測數據填入。
這些是 v0.1 實測。v0.2 離線與模擬測試結果見 docs/TEST_REPORT_zh-TW.txt。
