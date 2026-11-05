# MSU2 Mini Studio

[![macOS verification](https://github.com/xdxd-lab/msu2-mini-studio/actions/workflows/macos.yml/badge.svg)](https://github.com/xdxd-lab/msu2-mini-studio/actions/workflows/macos.yml)

MSU2 Mini（160 × 80）的原生 macOS C++ 應用程式：桌面投射、圖片串流、連線管理、外部素材 Flash 寫入與圖片下載。使用 AppKit、ScreenCaptureKit 及共用 C++17 核心，基本建置不需第三方套件。

目前為工程開發版本。Pair 串流仍有 ACK 逾時；素材寫入及正式簽章發行也有待驗收。請先閱讀[現況與待辦](docs/STATUS_zh-TW.md)。MCU 韌體燒錄尚未實作，介面顯示的主機更新率亦不等於 LCD FPS。

## 建置與啟動

需要 macOS 13 以上與 Xcode Command Line Tools。建置本機架構，尚未提供正式安裝包。

```sh
git clone https://github.com/xdxd-lab/msu2-mini-studio.git
cd msu2-mini-studio
BUILD_DIR="$PWD/results/local-build" bash msu2_macos_app/scripts/build.sh
open -n -a "$PWD/results/local-build/MSU2 Mini Studio.app"
```

此路徑與預設的 `msu2_macos_app/build` 分開，便於保留既有 App。CLI 位於同一 bundle 的 `Contents/MacOS/msu2ctl`。桌面投射需授權實際啟動的 App；預設 ad-hoc 簽章在重建後可能需重新授權。[操作說明](msu2_macos_app/README_zh-TW.txt)包含連線、Flash、下載與工作紀錄的用法。

## 開發驗證

```sh
python3 scripts/check_docs.py
BUILD_DIR="$PWD/results/local-tests" bash msu2_macos_app/scripts/test.sh
SANITIZERS=1 BUILD_DIR="$PWD/results/local-sanitized" bash msu2_macos_app/scripts/test.sh
```

文件檢查另需 Python 3。離線套件使用假傳輸及影像來源，不開啟裝置或擷取桌面；CI 的硬體 driver 步驟也只編譯連結。實機需另行安排單一序列埠擁有者與共用鎖。

## 導覽

| 入口 | 內容 |
|---|---|
| [交接入口](docs/HANDOFF_zh-TW.md) | 接手步驟、工作邊界、證據位置與下一步 |
| [文件索引](docs/README.md) | 現行文件與歷史調查的區別 |
| [軟體架構](docs/ARCHITECTURE_zh-TW.md) | 模組、執行緒、資料擁有權與時間定義 |
| [參與開發](CONTRIBUTING.md) | 修改、註解、驗證與提交規則 |
| [發布準備](docs/RELEASING_zh-TW.md) | 版本、簽章、授權範圍與尚待完成的驗收 |

產品程式在 `msu2_macos_app/`，共用核心在 `msu2_linux_v0_3/`。早期工具與 v0.2 原始碼因相互依賴保留既有路徑，屬於[歷史工具](docs/history/README.md)，不作為目前 App 的建置入口。實驗原始資料、App 及建置產物保留於本機，不隨 Git 推送。
