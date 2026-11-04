# MSU2 Mini 個人顯示測試工具 v0.2.0

這組工具只送 LCD 顯示資料，不寫入 Flash 或 MCU 韌體。它使用 Linux C++ v0.3
候選版的現有 `send`／`stream` 命令，不改局部更新演算法。可在目前的 macOS
工作區使用；Linux 上須先用當地 C++ 編譯器重新建置，並提供 `lsof`。

本版單張 `send` 未指定 `--hold-open-ms` 時等待 13 ms；明確指定 `0` 或其他值
可覆寫。預製影像新增數字編號輸入，例如 `--source 11` 等同 `--source 11_white`。
13 ms 是依目前 macOS 觀察選定的暫用預設，實際等待時間仍以日誌為準。

## 準備

在工作區根目錄執行：

```sh
bash msu2_linux_v0_3/scripts/build.sh
python3 manual_test_kit/make_images.py
python3 manual_test_kit/manual_test.py --list
python3 manual_test_kit/manual_test.py --help
python3 manual_test_kit/manual_test.py --version
./msu2_linux_v0_3/build/msu2 ports
```

`ports` 只列舉候選埠。每次重新插拔後，請重新確認實際埠；macOS 通常用
`/dev/cu.usbmodem...`，Linux 常見 `/dev/ttyACM0`，實際名稱以列舉結果為準。
同時關閉其他顯示／序列埠程式。工具在每次真跑前取得專案共用鎖並用 `lsof`
確認 macOS 的 cu／tty 兩個別名；被佔用或無法確認時不會開埠。每次只啟動一例，
看完結果再開始下一例。不要同時使用另一個測試程式。

## 影像

影像在 `manual_test_kit/images/`，均為原生 160×80 P6 PPM；同名 PNG 僅供
預覽。`contact_sheet.png` 按下表由左到右、由上到下排列。

`--source` 可直接用 `1` 到 `11`，亦支援 `01` 到 `09`；編號就是完整名稱
開頭的數字。例如 `1`／`01`／`01_color_bars` 選同一張，`11`／`11_white`
選同一張。`--list` 列出編號與完整名稱；完整名稱、內建圖樣與自訂 PPM 路徑仍可使用。

| `--source` | 用途 | 畫面應見到 |
| --- | --- | --- |
| `01_color_bars` | 顏色與全幅 | 白、黃、青、綠、洋紅、紅、藍、黑直條 |
| `02_geometry_grid` | 邊框、座標、方向 | 8px 格線；四角依序紅、綠、藍、洋紅 |
| `03_bands4` | 四橫帶邊界 | 紅、橙、藍、紫四層，位於 y=0/20/40/60 |
| `04_center3` | 中央與側區 | 藍色左 40px、黑白中央 80px、橙色右 40px |
| `05_halves2` | 左右半幅 | 藍色左 80px、橙色右 80px |
| `06_boundary_targets` | 區界與錯位 | 色塊跨 x=40/80/120；水平線在 y=20/40/60 |
| `07_checker1` | 細節與壓縮 | 黑白單像素棋盤 |
| `08_gradient` | RGB565 色階 | 彩色漸層；藍色通道的斜向回繞是預期圖案，並可能見到正常量化階梯 |
| `09_noise` | 高資料量 | 固定種子的全畫面彩色雜訊 |
| `10_black`／`11_white` | 清屏與殘影 | 全黑／全白 |

影像是靜態來源；對它執行 `stream --update diff --keyframe-ms 0` 時，首張之後通常
不再傳輸。要觀察連續局部更新，請選工具的內建 `moving`；高負載對照選 `noise`。
兩者由同一個 C++ 程式依來源 frame ID 產生，可重複比較不同參數。

執行中按 Ctrl+C 時，工具會先請 C++ 程式完成正在傳送的更新並寫出 CSV，
持續持有序列埠鎖直到子程序退出；只有超過寬限時間才會進一步終止。
因此中斷不一定會瞬間返回，結果狀態會標為 `interrupted`。

常用參數與預設值：

| 參數 | 作用與建議 |
| --- | --- |
| `--source` | 預製 PPM 編號 `1..11` 或名稱、自己的 PPM 路徑，或內建 `moving`／`noise` 圖樣 |
| `--action send/stream` | 單張顯示／連續測試；動態圖樣請選 `stream` |
| `--fps 45 --frames 450` | 目標排程 45 FPS、最多 450 個來源時槽；不是實測 LCD FPS |
| `--mode pair/raw` | 比較編碼與原始像素傳輸；每輪只改一項 |
| `--update diff/full` | 比較局部更新與全幅；`--keyframe-ms` 僅適用 `diff` |
| `--priority-mode off/bands4/center3/halves2` | 比較無分區與三種自適應方案，只適用 `stream` |
| `--chunk 60 --timeout-ms 2000` | 每次 payload 寫入最多 60 bytes；握手及 I/O 等待上限 2000 ms，先保持此基準 |
| `--ack prefix/echo` | 預設 `prefix`；只有已確認韌體支援時才使用 `echo` |
| `--orientation 0/1` | 需要時指定方向；省略時沿用程式的預設行為 |
| `--hold-open-ms 13` | 僅適用 `send`，省略時預設 13；送完 payload 後保持序列埠開啟 0..60000 毫秒，明確指定值可覆寫 |

例如直接送白圖，使用預設 13 ms：

```sh
python3 manual_test_kit/manual_test.py --port /dev/cu.usbmodem01234567891 \
  --source 11 --action send --label white_default13
```

要比較立即關埠，只加上 `--hold-open-ms 0`；測其他等待時間則指定該數值。
`stream` 省略此參數時不加入等待，指定非零值會被拒絕。預製 `8`、`9` 是
靜態漸層、雜訊 PPM；動態圖樣仍使用 `gradient`、`noise` 等完整 pattern 名稱。

## 單張送完後保持連線的對照

先重新建置 `bash msu2_linux_v0_3/scripts/build.sh`。兩輪使用相同色條、編碼與
傳輸參數，每輪先確認設備的初始畫面相同（例如原始花朵圖仍可見），再各自執行：

```sh
python3 manual_test_kit/manual_test.py --port /dev/cu.usbmodem01234567891 \
  --source 01_color_bars --action send --hold-open-ms 0 --label bars_hold0

python3 manual_test_kit/manual_test.py --port /dev/cu.usbmodem01234567891 \
  --source 01_color_bars --action send --hold-open-ms 10000 --label bars_hold10s
```

每次只啟動一輪。兩組都在開始後約 1 秒與 10 秒觀察底部；10 秒組也觀察
命令結束前後是否變化。請記錄有無重新插拔、初始畫面及殘留消失的時間。
前一輪已有完整色條時，下一輪傳輸不完整可能被原畫面遮掩，不能當作成功對照。

等待發生在 C++ 子程序內，payload 寫完後、序列埠關閉之前，共用鎖也保留到
子程序退出。`stdout.log` 保存 `post_send_hold_requested_ms`、實際等待時間與
中斷狀態，`metadata.json` 保存指定參數。等待不是 LCD 完成確認，也不會補送
影像；Ctrl+C 可提前結束等待。命令結束後才執行 shell `sleep` 無法保持序列埠開啟。

## 一次一例的操作

把下列 `--port` 改成目前 `ports` 顯示的實際名稱。先預覽命令；`--dry-run`
不開埠、不送圖：

```sh
python3 manual_test_kit/manual_test.py --port /dev/cu.usbmodem01234567891 \
  --source 01_color_bars --action send --dry-run
```

確認後送單張，觀察是否與 PNG 預覽一致：

```sh
python3 manual_test_kit/manual_test.py --port /dev/cu.usbmodem01234567891 \
  --source 01_color_bars --action send --label bars
```

再單獨測一張格線或區域影像，例如把 `--source` 換成 `02_geometry_grid`、
`04_center3`。確認靜態畫面後，以同一套參數各跑一次動態對照：

```sh
python3 manual_test_kit/manual_test.py --port /dev/cu.usbmodem01234567891 \
  --source moving --action stream --fps 45 --frames 450 --mode pair \
  --update diff --keyframe-ms 0 --chunk 60 --priority-mode off --label moving_off_45
```

要比較分區策略，逐次把 `--priority-mode` 改為 `bands4`、`center3`、`halves2`，
每次使用不同 `--label`。第一次先保持相同的來源、FPS、編碼與 chunk；再單獨把
`--fps` 改為 46、47、…、60，或把 `--mode` 改為 `raw`。不需要為了達到 60 FPS
而繼續加壓；遇到異常先保留該輪結果。`--source noise` 適合放大負載差異，
但不能用它判斷小區塊移動時的殘影。`03_bands4`、`04_center3`、`05_halves2`
只是靜態邊界標靶，不會因影像名稱自動切換策略；`--priority-mode` 選的是自適應
方案，是否真的進入分區請檢查 `stream.csv` 的 `active_layout` 與 `mode_event`。

預設 `--ack prefix`、`--chunk 60`、`--timeout-ms 2000`。`--ack echo` 只適合
已確認支援完整 echo 的韌體。macOS 既有測試曾在 3840-byte 區塊的第二個
視窗失敗，所以先用 60；若要研究 3840，請單獨標記為診斷，不把失敗與 FPS
混為一談。`--keyframe-ms 1000` 可另外觀察定期全幅，但與 `0` 是不同負載條件。

自己的 P6/P3 PPM 可直接用檔案路徑：

```sh
python3 manual_test_kit/manual_test.py --port /dev/cu.usbmodem01234567891 \
  --source '/完整路徑/my_picture.ppm' --action send --label own_image
```

程式會把非 160×80 的影像等比縮放並置中裁切；若要精確檢查像素或分區界線，
建議先做成 160×80、8-bit RGB PPM。PNG 預覽不能直接當 `--source` 輸入。

## 保存與判讀

每輪在 `manual_test_kit/results/` 建立唯一資料夾，保存 `metadata.json`（完整
參數、工具版本、命令、執行檔與影像 SHA-256、guard 證據、exit code）、`stdout.log`、
`stderr.log`、串流 `stream.csv`、檔案校驗清單，以及可填寫的
`OBSERVATION_zh-TW.txt`。即使握手失敗或被另一測試佔用，也會留下狀態；
後者不會啟動硬體傳輸。`passed` 只代表主機命令正常退出與既有 ACK 規則，
不代表 LCD 確實顯示完成。請在觀察檔填寫完整度、方向、殘影、缺塊、卡頓與
大約發生時間。

`metadata.source_requested` 保留原輸入，例如 `11`；`metadata.source` 保存完整
預製名稱 `11_white`，便於與舊紀錄比較。`hold_open_ms` 保存實際採用的設定，
`hold_open_ms_explicit` 區分使用預設與手動覆寫；實際等待仍記在 `stdout.log`。

串流 CSV 的 `dropped_total` 是等候來源被取代；分區模式另有 `region_skipped`
（主動略過）、`region_unchanged`、`region_missed_queue`。ACK 逾時或握手失敗
請看 `stderr.log` 與 `metadata.json`；失敗的未完成時槽不會假裝有成功 CSV 列。
原始 CSV 與主機日誌不能單獨證明實體 LCD FPS。

累積幾輪後可快速比較主機側結果：

```sh
python3 manual_test_kit/compare_runs.py
```

輸出可另存為 CSV；`completed_rows`、`replaced_at_last_row`、`side_skips` 和
`timeout_event` 都是主機紀錄。通用 `response_timeout_unspecified` 可能發生在
握手或其他等待階段，不能直接當成顯示視窗 ACK 逾時；仍須結合原始日誌與
每輪的目視觀察檔判讀。`none` 只表示沒有命中這兩種文字標記，不排除其他
階段失敗。`completed_rows` 為 `-` 表示沒有 CSV（例如單張或握手失敗），
`0` 才表示 CSV 已存在但沒有完成的來源時槽。
