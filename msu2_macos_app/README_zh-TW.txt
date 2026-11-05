MSU2 Mini Studio — macOS 操作說明

工程開發版本；桌面投射、圖片串流、外部素材 Flash 寫入與圖片下載。
目前功能與待驗收項目以原始碼根目錄 docs/STATUS_zh-TW.md 為準。
完整原始碼文件：https://github.com/xdxd-lab/msu2-mini-studio/tree/main/docs

開啟與建置
從原始碼根目錄執行：
  BUILD_DIR="$PWD/results/local-build" bash msu2_macos_app/scripts/build.sh
  open -n -a "$PWD/results/local-build/MSU2 Mini Studio.app"
需要 macOS 13 或以上、Xcode Command Line Tools。無須 Python、CMake、Qt 或第三方套件即可執行應用程式。
目前建置依本機架構產生執行檔，使用 ad-hoc 簽署，尚非 Developer ID 公證發行版本。
BUILD_DIR 決定實際輸出。未設定時預設為 msu2_macos_app/build，會覆寫該處 App；
launch_current.sh 只啟動此預設路徑，不會啟動上述 results/local-build 的隔離建置。
同名 App 容易被其他啟動方式選錯，請核對絕對路徑。
預設臨時簽章在重建後可能不被先前的螢幕錄製授權認可。備妥穩定簽章身分後可設定
MSU2_CODESIGN_IDENTITY；此設定不代表已完成公證或升版授權驗收。
核心使用 C++17；視窗、圖片解碼、ScreenCaptureKit 與 IOKit 透過 Objective-C++ 呼叫原生框架。

開始使用
1. 「重新整理」取得目前序列埠，再選擇 MSU2 候選裝置並按「連線」。
   USB 名稱、VID/PID 只提供辨識線索，必須完成 MSN banner + MSNCN 握手。
   此韌體協定沒有查詢螢幕尺寸，160×80 是 MINI 設定，非裝置回報尺寸。
2. 選擇「圖片」並載入 PNG、JPEG 或 PPM，可先看中央裁切後的 160×80 預覽。
   「傳送單張」維持現有 full/pair 格式，寫入後等待預設 13 ms，連線保留。
3. 桌面投射請先按「授權桌面擷取」，在 macOS 螢幕錄製權限中允許本程式。
   請確認授權的是實際啟動路徑的版本；必要時完全關閉舊版及目前版，再從精確路徑重開。
   重新建置臨時簽章版本後可能需重新核准。擷取主顯示器中央 2:1 範圍，縮放為 160×80，包含游標、不錄音。
   權限只由使用者點擊申請；程式啟動不會自動擷取桌面。
4. 「開始串流」持續執行，直到「停止」。暫停保留連線，恢復後第一張重新全幅同步。
   預設 30 FPS、diff、pair、60-byte chunk、2000 ms I/O timeout，分區優先預設關閉。
   FPS 是來源排程目標；主機送出率與 LCD 可見刷新率不同。60 FPS 是可選上限，非性能保證。
5. 選擇四橫帶、中央三區或左右兩區，可使用既有 PriorityScheduler 自動降載／恢復。
   保留原有 DeltaPlanner 與頁編碼，沒有另外改寫顯示更新演算法。
   來源只留最新等待影格；傳輸中的矩形依序完成，不堆積過時畫面。
   中央三區維持中央原排程，壓力高時略過側區；略過、來源缺幀、ACK 錯誤分開記錄。
6. 修改設定後停止再開始即可套用。自動重連預設關閉；啟用後每次串流最多三次，
   只嘗試相同埠及相同可得 VID/PID/USB 序號。握手後全幅重建參考，不延續未知差分狀態。
   同一連線在無命令活動超過 4.5 秒後，下次傳送前會等待 MSN 並重新驗證握手；
   4.5 秒是本程式的保守檢查門檻，不是量測所得韌體常數。此時可暫時顯示連線中。

圖片素材燒錄
「圖片燒錄」處理外部 P25D80 素材 Flash，並非 MCU 韌體更新。
目前一次燒錄一張 160×80 RGB565 圖，共 100 頁，每頁 256 bytes。
MINI 文件描述 4096 頁 / 1 MiB；開始頁可選 0..3996。
3826（背景）、3926（相簿）與 0（動畫首張）為歷史素材布局參考，不能假定為空白區或適用所有韌體。
選圖後預覽，確認對話框顯示實際埠、圖片與覆寫頁範圍；按下確認才執行。
確認後使用同一份已解碼像素，避免檔案在預覽後變動而寫入另一張圖片。

每次燒錄流程：
  範圍驗證 → 嚴格位址回覆讀取 → 備份涵蓋的 4 KiB sector 範圍與相鄰資料 → 保存備份與預定像素
  → 擦除指定頁 → 逐頁寫入 → 全部 25,600 bytes 讀回比對 → 相鄰備份資料比對。
備份或讀回格式不相容時，在擦除前停止。寫入不自動重試或斷點續寫。
「取消燒錄」會在可停止的命令／頁邊界退出；取消後可能已有部分內容改變。
備份保留在本次紀錄資料夾，格式為原始 bytes，旁附起始 byte 位址 manifest。
目前沒有自動還原按鈕，不能把備份誤解為已驗證的一鍵還原功能。

Flash ACK/位址回覆採較嚴格的相容性檢查；其實體韌體支援與相鄰頁擦除語意尚未經本版實機寫入驗證。
如果不相容會停止並保存證據，不能放寬檢查而假裝成功。實作依據見 docs/FLASH_PROTOCOL.txt。

圖片下載
「圖片下載」讀取裝置外部 Flash 中儲存的圖片素材，提供背景3826、相簿3926、
動畫首張0與自訂起始頁。請依實際素材配置選擇；這些預設值不是裝置查詢結果。
起始頁範圍0..3996，一次讀取100頁（25,600 bytes），以高位元組在前的RGB565解讀。
指定儲存頁與LCD目前顯示的畫面不一定相同；協定沒有提供目前顯示頁的查詢。
先連線，選擇起始頁與下載資料夾，再按「讀取並下載圖片」。
每次建立獨立子資料夾，完整成功後包含image.png、image.rgb565與manifest.txt。
原始RGB565保留完整讀回bytes；PNG只將其展開為RGB888，維持160×80尺寸。
每個byte的回覆都必須核對地址，支援進度及取消。讀取逐byte執行，可能需要等待。
讀取期間不會執行擦除、寫入或顯示命令，也不會同時串流。
取消或錯誤時關閉連線，移除未完成圖片，保留狀態紀錄；不會自動重試。
詳細說明見docs/FLASH_DOWNLOAD_zh-TW.txt。

工作紀錄
視窗「工作紀錄」可開啟：~/Library/Logs/MSU2 Mini Studio/<日期_PID_序號>/
  events.log          連線狀態、握手、ACK 逾時、其他傳輸錯誤與重連
  stream_N.csv        來源幀號、擷取／處理／ACK／寫入耗時、bytes、來源缺幀、分區排程／略過
  flash_*.txt         寫入範圍、結果、錯誤與備份起始位址
  flash_*_backup.bin  覆蓋範圍及涵蓋 sector 的原始備份
  flash_*_intended.rgb565  使用者確認的圖片像素
預覽是來源畫面，不是 LCD 光學擷取；既有 prefix ACK 也不是面板已顯示完成的證據。
「來源擷取丟幀」是排程錯過的時槽，「等待影格取代」是單槽佇列覆蓋的等待影格；
「傳送觀察缺幀」是消費端觀察的來源幀號缺號。三者不能相加成 LCD 遺失影格。
預覽的自動對比外框不改變送出或燒錄像素，預覽也不是 LCD 回讀。

序列埠互斥
C++ SerialGuard 與 scripts/macos/serial_guard.py 使用相同 cu/tty 正規化、SHA256 前20字元 lock key、flock。
每次開埠前檢查兩個別名是否被占用；已有擁有者時不開埠、不終止對方。
在原專案中共用 results/macos/.device-locks；搬走且原專案不存在時用 Application Support 下的 device-locks。
可設 MSU2_SHARED_LOCK_ROOT 讓不同副本或其他啟動器共用固定目錄。

命令列伴隨程式（與視窗共用同一控制層）
以下路徑對應前述隔離建置；其他 BUILD_DIR 請換成實際路徑。
  "results/local-build/MSU2 Mini Studio.app/Contents/MacOS/msu2ctl" --help
  .../msu2ctl devices
  .../msu2ctl connect --port /dev/cu.usbmodem01234567891
  .../msu2ctl send --port /dev/cu.usbmodem01234567891 --input manual_test_kit/images/01_color_bars.ppm
  .../msu2ctl stream --port /dev/cu.usbmodem01234567891 --source moving --fps 30 --duration 10
  .../msu2ctl stream --port /dev/cu.usbmodem01234567891 --source desktop --fps 30
  .../msu2ctl download --port /dev/cu.usbmodem01234567891 --start-page 3826 --output-dir /path/to/downloads
省略 --duration 或設 0 時串流直到 Ctrl-C。Flash 寫入由圖形視窗確認後執行。

離線驗證
  BUILD_DIR="$PWD/results/local-tests" bash msu2_macos_app/scripts/test.sh
  SANITIZERS=1 BUILD_DIR="$PWD/results/local-sanitized" bash msu2_macos_app/scripts/test.sh
測試包含既有核心/差分/分區測試、fake transport 的連線/暫停/錯誤分類/鎖釋放、Flash 命令及完整讀回。
這些離線測試不等於真實螢幕顯示或 Flash 持久性驗收。
實機測試與分析依專案 AGENTS.md 分工，原始證據另外保存於 results/macos，不隨 Git 推送。
接手請讀根目錄 docs/HANDOFF_zh-TW.md；歷史驗證不可當成目前任一新建置的實機驗收。
