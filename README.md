# MSU2 Mini Studio

原生 macOS C++ 應用程式，提供 MSU2 Mini（160 × 80）的桌面投射、圖片／圖樣串流、連線管理、外部素材 Flash 寫入及圖片下載。

目前為工程開發版本。Raw 全幅已有約 24 次／秒的主機更新量測；Pair 在 24／28 FPS 的短測仍有視窗 ACK 逾時，根因尚待釐清。主機傳送完成與 LCD 實際顯示不同，不能將介面速率稱作面板 FPS。MCU 韌體燒錄尚未實作。

## 建置與離線驗證

需要 macOS 13 以上與 Xcode Command Line Tools，使用 C++17、Objective-C++ 及系統原生框架；macOS App 的基本建置不需要 CMake 或第三方套件。從儲存庫根目錄執行：

```sh
bash msu2_macos_app/scripts/build.sh
bash msu2_macos_app/scripts/test.sh
bash msu2_macos_app/scripts/launch_current.sh
```

建置產物為 `msu2_macos_app/build/MSU2 Mini Studio.app`，同一 bundle 內含 `Contents/MacOS/msu2ctl`。執行 `msu2ctl --help` 可查看 CLI。`launch_current.sh` 只啟動上述固定路徑。

在既有工作區執行預設建置會覆寫固定路徑的 App；保留已授權版本時，請使用下面的獨立 `BUILD_DIR`。

開發驗證使用獨立建置目錄：

```sh
BUILD_DIR="$PWD/results/local-check/build" bash msu2_macos_app/scripts/build.sh
BUILD_DIR="$PWD/results/local-check/tests" bash msu2_macos_app/scripts/test.sh
SANITIZERS=1 BUILD_DIR="$PWD/results/local-check/sanitized" bash msu2_macos_app/scripts/test.sh
```

離線測試使用 fake transport／來源，不會打開 MSU2 序列埠；圖片框架測試不擷取桌面。硬體測試不屬於自動離線驗證。

## 授權與診斷

桌面擷取需要在系統隱私設定允許 App。預設使用 ad-hoc 簽章，重建可能使授權失效；若備妥穩定簽章身分，可設定 `MSU2_CODESIGN_IDENTITY` 再建置。多份同名 App 容易被選錯，請核對絕對路徑。Git 不包含舊的本機候選 App，新的 checkout 必須重新建置。

設定 `MSU2_DIAGNOSTICS=1` 可啟用背景診斷，`2` 額外記每個 payload 寫入的長度與耗時。記錄包含設定、階段、時間、已識別回覆樣式及有限的讀取分類，不保存桌面像素或未知接收內容。正常退出後的 `log,end`、遺失計數及寫檔狀態用於檢查紀錄完整性。詳見[診斷說明](docs/STREAM_DIAGNOSTICS_20261030_zh-TW.md)。

## 原始碼位置

| 路徑 | 用途 |
|---|---|
| `msu2_macos_app/` | 原生介面、控制器、ScreenCaptureKit／圖片橋接、序列埠鎖與素材 Flash 功能 |
| `msu2_linux_v0_3/` | 共用協定、Session、傳輸、影像、差分與分區核心，以及獨立 CLI |
| `scripts/macos/serial_guard.py` | 與本機 App 共用鎖位置及別名規則的實驗協調工具 |
| `scripts/`、`tests/`、`manual_test_kit/` | 歷史診斷工具、離線測試與合成測試圖片 |
| `MSU2_MINI_CPP_v0_2_20261010/`、`msu2_cpp_v0_2` | 歷史原始碼與相對連結；部分舊工具仍依賴它們 |
| `docs/`、`handoff/` | 設計／驗證文件及本機交接歷史 |
| `reference/` | 原始研究來源描述；下載的 PDF 及外部量測檔保留於本機 |

歷史診斷工具可能需要先建置其指定的舊版工具；不應直接將它們作為目前 App 的測試入口。共用核心的 CMake／獨立 CLI 路徑未在本輪驗證，本輪驗證範圍是 macOS 建置腳本及其離線套件。

[架構與執行緒說明](docs/ARCHITECTURE_zh-TW.md)、[操作說明](msu2_macos_app/README_zh-TW.txt)、[Git 交接與證據索引](docs/REPOSITORY_WORKFLOW_zh-TW.md)。

## 硬體與測試邊界

同一時間僅能有一個序列埠擁有者。實機前須檢查 cu／tty 兩別名與共用 guard，凍結執行檔並保存前後 SHA256；執行時不要重建或覆寫該執行檔。素材 Flash 寫入會變更裝置資料，必須先確認圖片、頁範圍及備份；實體擦寫、相鄰資料保留與斷電持久性尚未完成全面實機驗證。

原始實驗資料、App、建置物件與本機結果放在 `results/` 等被忽略的目錄。文件中的本機證據連結在乾淨 checkout 可能不存在；提交文件摘要不代表原始證據已上傳。歷史結論須連同日期及限制閱讀，優先參考最新驗證紀錄。

App 在工作區建置時會使用該專案的鎖目錄；不同 clone 或搬移後的二進位不保證共用同一目錄。同機協同操作可明確指定相同的 `MSU2_SHARED_LOCK_ROOT`，並仍須檢查 cu／tty 擁有者。

現有授權檔位於共用核心及歷史 v0.2 目錄；本次沒有替其餘程式指定新的發布授權。
