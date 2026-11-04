MSU2 Mini Studio — macOS C++ 應用程式（目前為淺色介面，含圖片下載與串流速率分項）

目前原始碼入口與建置說明見 README.md；架構見 docs/ARCHITECTURE_zh-TW.md。
本檔保留歷史調查過程；其中模型、測試狀態與本機候選路徑須按日期判讀。

原生視窗程式：msu2_macos_app/build/MSU2 Mini Studio.app
建置：bash msu2_macos_app/scripts/build.sh
開啟：bash msu2_macos_app/scripts/launch_current.sh
操作說明：msu2_macos_app/README_zh-TW.txt
目前介面驗證：results/macos/metrics_verify_20261024/ui/elevated-smoke.png
既有實機驗證：msu2_macos_app/VALIDATION_zh-TW.txt

2026-10-30 軟體品質第一批：已完成重連／喚醒修正、差分規劃去重、離線及 ASan/UBSan 驗證。
候選版：results/macos/software_quality_20261030/reviewed_build/MSU2 Mini Studio.app
審核與效能證據：docs/SOFTWARE_QUALITY_REVIEW_20261030_zh-TW.md
後續計畫及本次 Herdr 分工：docs/SOFTWARE_QUALITY_PLAN_20261030_zh-TW.md
上述候選尚未實機驗證；原固定路徑 App 保留，不由 launch_current.sh 切換至候選版。

專案留有多份同名的歷史測試與深色介面 App。請使用上述固定路徑腳本，
避免按名稱搜尋開到舊版。舊版介面文件仍保留於 msu2_macos_app/docs/DARK_TOUCH_UI_VALIDATION.txt。
目前的臨時簽章會在重新建置後改變 macOS 辨識身分，已勾選的螢幕錄製授權可能因此不適用新版。

支援圖片素材 Flash 燒錄、主桌面投射、圖片／圖樣串流與連線管理。
C++17 控制層重用既有 diff/pair/priority 核心，原生介面使用 Objective-C++。
目前工作範圍與代理分工請以 AGENTS.md 為準；下方保留早期測試階段紀錄。

MSU2 Mini — macOS 測試工作區
2026-10-11，Asia/Taipei

後續開發：Linux v0.3 候選版位於 msu2_linux_v0_3/，已依另一工作
01a12629-3ee2-7f13-94b2-12133b6828cf 的共識加入
--priority-mode off|bands4|center3|halves2。該目錄有獨立原始碼、建置腳本與
README_zh-TW.txt；本文件下方保留既有 macOS 排查紀錄與當時的工作階段目標。
三種分區模式處理主機更新負荷，不能當成閒置會話 ACK 或 3840-byte 故障的修復。
Linux v0.3 這輪測試依最新要求交由 GPT-6 Luna／高，全面分析交 GPT-6 Astra／Ultra；
下方「Luna／中」僅記錄較早 macOS 實機測試的執行設定。
Linux v0.3 原始碼包位於 dist/msu2_linux_v0_3_source_20261011.tar.gz，SHA-256：
d592bf6400dae95a81e7cafa4c7b3838d3a7039f31470ad448a73f54c9d97ff2。
最終離線測試證據位於 results/linux_v0_3/20261010T185938Z_macos-arm64/：build、
core、delta、priority 與完整 POSIX PTY 均通過；僅在 macOS arm64 執行，未做 Linux
原生執行或 MSU2 實機驗證。全面分析見 docs/LINUX_V03_ASTRA_ANALYSIS_20261011_zh-TW.txt。

目標
本段紀錄最初在目前 Mac 驗證 C++ v0.2 的全幅與局部 LCD 更新，建立可重現的效能基準。
Windows x64、Rust 的開發仍待後續評估；Linux 的跨幀區域排程已另起 v0.3 候選版。
這些測試操作 LCD 顯示，不是 Flash 或 MCU 韌體燒錄。

本輪排查更新
- 移動→靜止→恢復局部更新六案已完成，見 docs/STATIC_TRANSITION_RESULTS_20261011_zh-TW.txt；
  全面分析見 docs/STATIC_TRANSITION_ASTRA_ANALYSIS_20261011_zh-TW.txt。
  45FPS靜止3秒兩個對照恢復正常、零掉幀；45/60FPS靜止6秒均在第一個resume window
  ACK逾時，讀到20個MSN01。靜止期間仍擷取，但沒有serial讀寫。
  兩個隔離副本的單次嚴格重握手+全幀恢復成功，接續局部更新至末幀；原fault保留。
  約2.07秒卡頓中，原ACK等待占約2秒；恢復新增45FPS/92、60FPS/123次等待幀取代，
  後者另有startup1，總124。這些集中於等待的drop不等同穩態FPS門檻。
  正式原始碼及更新算法未變；新trace/helper/runner在tests、scripts，生成主程式/binary在results。
  本輪沒有新的LCD目視確認，不代表3840或歷史連續移動中的偶發ACK問題已解。
  方法、source mapping、RX時刻與主機接受界線見 docs/STATIC_TRANSITION_TEST_PLAN_20261011_zh-TW.txt。
- 新會話／3840／恢復實機覆核見 docs/SESSION_STATE_RESULTS_20261011_zh-TW.txt，
  全面分析見 docs/SESSION_STATE_ASTRA_ANALYSIS_20261011_zh-TW.txt。
  同fd兩次僅RX閒置約5.114秒就收到MSN01；舊會話開窗逾時後，完整握手與兩張full恢復成功。
  完全無讀寫30秒也能同fd握手恢復；每5秒送full、持續30秒的對照沒有公告且全部成功。
  原版60 FPS／60秒完成3600/3600、0掉幀；3840的0/100ms間隔都仍失敗，後續握手亦失敗。
  3840後的原版案例未通過初始握手，沒有串流樣本；不能算60 FPS運行失敗或通過。
  方法、計時錨點及observer effect見 docs/SESSION_STATE_TEST_PLAN_20261011_zh-TW.txt。
  Astra另指出keyframe_ms=0的靜止畫面會停止TX；高擷取FPS不保證會話保持。
  該報告當時列為待測的「靜止後再變動」，現已由上列六案驗證。
- 2026-10-11閒置／重連覆核見 docs/IDLE_ACK_RESULTS_20261010_zh-TW.txt：
  原版60 FPS三個30秒案例與45控制全完成；同連線5秒閒置full成功，30／60秒後
  開窗ACK逾時但持續收到MSN01公告，較支持投屏會話閒置失效／回到握手狀態的假說。
  3840第二次full仍逾時，兩秒完全無RX，需與閒置後MSN公告分別排查。
- 45～60 FPS逐級實測與額外技術資料的判讀見 docs/FPS_SWEEP_20261010_zh-TW.txt。
  初掃45～58零掉幀；59、60僅起始各取代1張等待幀，16輪都沒有ACK逾時。
  追加確認因命令授權等待中止而未計入；收尾已確認cu/tty無持有者、合作鎖已釋放。
- ACK／3840／重連的前輪結果與改善建議見 docs/ACK_INVESTIGATION_RESULTS_20261010.txt；
  GPT-6 Sol 的實機交接明細見 docs/MACOS_HARDWARE_DIAGNOSTICS_20261010.txt。
- 依使用者最新設定，硬體由 GPT-6 Luna／中子代理執行，完成後全面分析交 GPT-6 Astra／Ultra，
  主代理覆核與彙整；一次只允許一個測試持有設備。
- 45 FPS／chunk60 的兩輪30秒都完成1350/1350、0掉幀，其中一輪使用者已確認畫面正常。
  60 FPS仍有成功／逾時交錯；3840在色條及小payload單色都失敗，重連尚未全面通過。
- baseline 新增 moving_diff_45（450 張）；分析器保留失敗前已完成 CSV 的統計，
  缺少最終摘要仍判 FAIL。runner 在每案例前後驗證執行檔 hash。
- 更新後測試工具的 42 項 Python 單元測試全部通過，另有idle/session-state probe FakeTransport
  self-test與probe實際C++產生CSV的17欄解析檢查通過；static-transition fake驗證MSN gate、
  strict echo、reference invalidate/full、partial failure、一次恢復上限及實際C++trace解析。
  硬體runner共同使用cu/tty
  別名鎖及lsof檢查；異常退出時先回收child才釋鎖，避免本工作區的測試重疊。

第一輪狀態（後續新增結果以上述報告為準）
- 已閱讀 ChatGPT「MSU2 Mini 前置調查討論」，並保存它的 Windows v0.2 參考紀錄。
- 本機 macOS 26.2、arm64、Apple Clang 21、Python 3.9.6。
- v0.2 原始套件已複製到本工作區，CHECKSUMS.sha256 的 57 項全部符合。
- MSU2 C++ 已原生建置為 Mach-O arm64。release core／delta／PTY 全部通過；
  ASan／UBSan 核心測試通過。12 組離線 diff-bench 的確定性數字與原 Linux 基準一致。
- 新增 macOS 測試工具的 16 項 Python 單元測試全部通過，兩份 shell script 語法檢查通過。
- 未修改原始 C++，指定 --chunk 60 已完成色條 30 秒（450/450、0 掉幀）與
  moving/diff 30 FPS（300/300、0 掉幀、30.0824 host FPS、215730 bytes）。
  使用者已確認「色條完整且穩定」及「移動正常，沒有明顯殘影或缺塊」。
- 預設 --chunk 3840 尚未驗收：數次測試完成首張後，第二個 window 的 ACK 逾時。
  --chunk 60 為目前已驗證的測試參數；連續重開程序可能沒有 MSN banner，
  手動拔插後偶爾仍有 MSNCN echo 逾時。重連、重複測試及長時間穩定性仍待完成。
- moving/diff 60 FPS、chunk60 在完成 496 次更新後 ACK 逾時，無最終摘要，
  判定 FAIL，待修復與重驗。本輪不自動進入 extended／soak，未完成 macOS 全面驗收。
- 實測埠為 /dev/cu.usbmodem01234567891；重新插拔後仍應重新列舉。
- CMake 尚未安裝；既有 bash/Clang 建置路徑不需要先安裝 CMake。

原始碼準備
已保存原始套件於：
  MSU2_MINI_CPP_v0_2_20261010/msu2_cpp_v0_2
工作區根目錄的 msu2_cpp_v0_2 為指向該資料夾的符號連結，可直接使用：
  msu2_cpp_v0_2/src/main.cpp
  msu2_cpp_v0_2/scripts/build.sh
  msu2_cpp_v0_2/tests/run_tests.sh
不要把套件內的 Linux ELF 執行檔當成 macOS 執行檔。
已核對解壓後 CHECKSUMS.sha256 的 57 項；這不等於核驗 ZIP 本身。
前次交付 ZIP 的 SHA256（由前次工作紀錄取得，尚未核對本機 ZIP）：
  62b17d4759e66dd7911853a261058c521764253ac96925178fb499341e582476

從本工作區執行
  python3 scripts/macos/preflight.py
  bash scripts/macos/run_offline.sh

原始碼在別處時：
  bash scripts/macos/run_offline.sh '/完整路徑/msu2_cpp_v0_2'
上述離線程序依序建置、跑既有 core/delta/PTY 模擬、ASan/UBSan 核心測試、diff-bench。
ASan 的 leak detection 關閉；不宣稱完成洩漏檢測。PTY 模擬不能取代實機。
已完成的 native 離線紀錄位於 results/macos/20261010T220832_offline_9YljT7。
原 PTY 程式最末行固定寫「Windows/macOS runtime were not exercised」；
這是原測試的過時文字，此次確實在 macOS 原生執行，仍未由 PTY 驗證實體 LCD。
首次本機執行若遇到沙盒拒絕，需要針對被拒絕的操作授權，不能把拒絕當成測試通過。

接上設備後
  ./msu2_cpp_v0_2/build/msu2 ports
把實際 /dev/cu.* 埠帶入下列命令；範例埠名只供對照，不保證重新插拔後仍相同：
  python3 scripts/macos/run_hardware.py --port /dev/cu.usbmodem01234567891 --suite visual --case bars_visual_30s --repeats 1 --chunk 60 --dry-run
  python3 scripts/macos/run_hardware.py --port /dev/cu.usbmodem01234567891 --suite visual --case bars_visual_30s --repeats 1 --chunk 60
  python3 scripts/macos/run_hardware.py --port /dev/cu.usbmodem01234567891 --suite baseline --case moving_diff_30 --repeats 1 --chunk 60
  python3 scripts/macos/run_hardware.py --port /dev/cu.usbmodem01234567891 --suite baseline --case moving_diff_45 --repeats 1 --chunk 60

若使用其他原始碼位置，加上 --binary '/完整路徑/msu2_cpp_v0_2/build/msu2'。
每次只選一個案例、一次重複，先確認色條，再測局部更新；上述命令不要直接串接執行。
本輪已測過多次不拔插重新開埠，但仍有握手失敗；不要把重開等同恢復成功。
若失敗，先保留該輪結果與錯誤階段，再另起有紀錄的新輪；必要時才手動拔插並重新列埠。
測試時關閉其他占用序列埠的上位機。runner 不自動重試、不自動重連；
握手或傳輸失敗會停止並保留 log。拔插後首次握手也可能失敗，原因仍待排查；
需人工確認後另啟一輪，使用新的結果目錄，不覆蓋舊資料。
專用run_fps_sweep.py則把各FPS視為獨立程序：單檔ACK／握手失敗保留後，
待程序退出才測下一檔，沒有同檔自動重試；占用鎖或沙盒拒絕會中止整個掃描。
runner 與原 C++ 的預設 chunk 仍是 3840；目前真跑請明確指定 --chunk 60。
完整 baseline 三輪、extended、soak 與重連排查尚未完成，不能稱 macOS 全部驗收通過。
--desktop 在既有 v0.2 只支援 Windows；本輪以內建圖樣／PPM 驗證。

僅供診斷復現，尚未通過的案例
下列 60 FPS 案例曾中途 ACK 逾時，先保留為待修與重驗，不列為正常操作建議：
  python3 scripts/macos/run_hardware.py --port /dev/cu.usbmodem01234567891 --suite baseline --case moving_diff_60 --repeats 1 --chunk 60
chunk3840 的第二個 window ACK 問題也僅供後續診斷；不要將參數改回 3840
當作已驗證設定。正式原始 C++ 與演算法未修改，生成的診斷主程式與binary留在 ignored results；
支援的測試工具保留在scripts/macos、tests/macos。

結果與判讀
每輪存於 results/macos 的唯一目錄，包含 CSV、log、manifest.json、目視確認清單。
使用 runner 印出的 hardware 目錄：
  python3 scripts/macos/analyze_results.py '/該輪結果/hardware' --output '/該輪結果/summary.json'
不要把 offline 的 diff_benchmark_macos.csv 當成 stream CSV 分析。
分析器的 PASS 只代表紀錄與計數一致；螢幕色彩、殘影、撕裂仍需目視／影片確認。
host_send_fps 是主機送出頻率，不是 LCD 可見更新率。

詳細矩陣、驗收與後續工作見 docs/MACOS_TEST_PLAN_zh-TW.txt。
逐級掃描命令（僅由指定Luna中子代理依序操作硬體）：
  python3 scripts/macos/run_fps_sweep.py --dry-run
  python3 scripts/macos/run_fps_sweep.py --label ascending
靜止/恢復診斷（真跑僅交由指定子代理，一次一個suite）：
  python3 scripts/macos/build_static_transition_diagnostics.py
  python3 scripts/macos/run_static_transition_suite.py --dry-run
  python3 scripts/macos/run_static_transition_suite.py --label initial
build會覆寫static_transition_tools中的診斷binary；保全現有紀錄時需先保留原工具與hash，
若重新build，舊run的hash索引仍只適用當時binary，不可把新工具冒稱原測試版本。
只有兩個標示recovery的案例使用隔離副本的單次恢復；正式run_hardware.py與原msu2不自動重連。
前次 Windows 原始參考資料見 reference/windows_v02，來源見該目錄 SOURCE.txt。
