# 發布準備

本儲存庫目前提供工程原始碼，尚未建立正式發行流程。以下項目是發行工作的一部分，不代表已完成；目前問題與優先順序見[現況](STATUS_zh-TW.md)。

## 版本與可追溯性

App 版本來源是 [Info.plist](../msu2_macos_app/resources/Info.plist) 的 `CFBundleShortVersionString` 與 `CFBundleVersion`。改版時一起維護版本、建置編號與說明；不要只改 README 或把歷史候選重新命名為新版。

目前 CLI 另有寫在程式中的版本標頭，尚未統一版本來源；在整合前需同步核對 CLI 與 App 說明。

每個發行候選須對應明確 commit、乾淨來源、工具鏈、CPU 架構及建置指令，保留 App／CLI SHA256 與驗證結果。現有建置腳本產生本機架構，不能將單一 arm64 建置稱為通用版本。

現有建置使用 `-g`，除錯符號可能留在 bundle 內；發行工作須將 dSYM 分開保存。實際 Developer ID 流程還需逐一簽署內含的 CLI 與 App，再驗證整個交付物；目前只驗證過預設 ad-hoc 路徑。

驗證候選使用新的 `BUILD_DIR`。現有腳本會重用輸出目錄，部分連結使用物件檔萬用字元，不能把既有目錄直接視為乾淨建置；日後應收斂來源清單及輸出生命週期。

## 簽章與授權升版

預設為 ad-hoc 簽章。`MSU2_CODESIGN_IDENTITY` 只選擇簽章身分，不會建立憑證，也不會自動完成 hardened runtime、公證、staple 或安裝包驗證。正式發行需另外定義及驗證完整流程。

維持已選定的 bundle identifier，除非有明確移轉方案；隨意改識別碼會影響 App 識別與隱私授權。升版驗收須從使用者實際啟動路徑檢查程式、簽章、螢幕擷取權限與重新啟動，涵蓋系統啟動到同名舊版的已知問題。

## 功能與穩定性驗收

離線 CI 是基本條件。發布前另需明確的支援 macOS／CPU 範圍、長時間串流與斷線恢復、分區桌面時效，以及素材寫入／還原／持久性的實機證據。公開說明要分開列「已有功能」與「已驗收情境」，不能用預期 FPS 代替量測。

## 授權與封裝內容

共用核心的授權原文位於 [msu2_linux_v0_3/LICENSE](../msu2_linux_v0_3/LICENSE)，歷史 v0.2 另有自己的 LICENSE 與 Windows runtime 授權副本。這次整理保留各檔原文，沒有替其餘 App 程式指定根目錄授權，也沒有據此宣告整個產品可商業散布。

發布前由權利持有人確認整體授權範圍、素材來源與第三方 notices，並將適用文件放入實際發行物。版本 tag、GitHub Release 與下載包應在這些條件及驗收完成後建立；目前 CI 不上傳 App 或自動發布版本。

## 本機簽章與封裝預檢

以下只讀命令針對**已選定的候選副本**；不啟動 App、不取得螢幕授權、不存取裝置，也不簽署或上傳。先將 `BUNDLE` 換成實際絕對路徑，避免同名 App 混用。輸出連同每步退出碼存入本機實驗目錄；一項失敗就保留失敗，不能用後續成功覆蓋。

```sh
BUNDLE="/absolute/path/to/candidate/MSU2 Mini Studio.app"
/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$BUNDLE/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$BUNDLE/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$BUNDLE/Contents/Info.plist"
/usr/bin/lipo -archs "$BUNDLE/Contents/MacOS/MSU2MiniStudio"
/usr/bin/lipo -archs "$BUNDLE/Contents/MacOS/msu2ctl"
/usr/bin/codesign --verify --strict --verbose=2 "$BUNDLE/Contents/MacOS/MSU2MiniStudio"
/usr/bin/codesign --verify --strict --verbose=2 "$BUNDLE/Contents/MacOS/msu2ctl"
/usr/bin/codesign --verify --deep --strict --verbose=2 "$BUNDLE"
/usr/bin/codesign -d --verbose=4 "$BUNDLE"
/usr/bin/codesign -d --verbose=4 "$BUNDLE/Contents/MacOS/msu2ctl"
/usr/bin/shasum -a 256 "$BUNDLE/Contents/MacOS/MSU2MiniStudio" "$BUNDLE/Contents/MacOS/msu2ctl"
```

預期 bundle ID 固定為 `local.msu2mini.studio`。版本取自候選 plist，不能以資料夾名稱推斷；本文件不自動升版。核對完整 bundle 檔案清單，對新增的 Mach-O、framework、helper 或 plug-in 也逐一驗簽，不能只驗頂層 bundle 就宣稱內含程式全部已驗證。簽章資訊分別記錄 Identifier、Authority／ad-hoc、TeamIdentifier 與 runtime flags；`codesign --verify` 成功不代表 Developer ID、公證、Gatekeeper 接受或 TCC 權限保留。

**先完成內容整理和簽署，再記錄最終 hash 並封裝。** 清理只能在另建的 staging 副本進行，不刪原候選、不改正式 App；需要改動封裝內容時重新驗簽。重新簽署可能改寫 GUI 或 CLI binary，須保留前後各自 hash，不能要求跨重簽相同。

### dSYM 與封裝清單

- 保存建置 commit、來源 manifest、工具鏈、架構、完整命令及退出碼；保留 GUI／CLI 各自 dSYM 到符號封存區，不混進給使用者的 App。
- 分別執行 `xcrun dwarfdump --uuid`，比對兩個 executable 與各自 dSYM 的 UUID／架構。只有 UUID 相符才可把它當成該 binary 的符號檔；dSYM 外移不代表 binary 已 strip。簽署後再次核對，不能只核對 GUI。
- 發行物只包含審閱過的 App、適用 notices／授權與使用說明。逐項檢查壓縮檔內容與解包結果，排除 `.DS_Store`、AppleDouble `._*`／`__MACOSX`、dSYM、物件檔、來源快照、測試日誌、憑證／私鑰及使用者素材；不要只用敏感檔名搜尋代替清單審查。
- ZIP 必須保留頂層 `MSU2 Mini Studio.app/`。用全新解包目錄檢查 symlink／檔案模式與完整相對路徑清單，逐檔核對最終 staging 與解包後 bytes/hash，並重做 GUI、CLI 與 bundle 的嚴格驗簽。另存 ZIP 本身 SHA256。
- 如未 strip，清楚標示；若另行決定 strip，必須保留匹配符號檔，在最終簽署之前完成，並重新驗證。不要為了檔案較小改寫既有凍結候選。

2026-11-12 的本機演練顯示：as-built ZIP 可解包驗簽且 binary hash 相同，但仍帶有 27 個 AppleDouble 檔與兩份 dSYM；整理副本後重新簽署使 GUI hash 改變。這證明「可驗簽」和「封裝內容乾淨」是不同檢查，亦不能把整理後的 ad-hoc 工程包當作正式交付物。原始清單與失敗封裝留在本機證據，不隨公開原始碼散布。

### 簽章身分不足時的停止條件

只讀盤點可用 `security find-identity -v -p codesigning` 及 `xcrun --find notarytool`。工具存在不等於有憑證、有效公證帳號或已取得票證；沒有適用 Developer ID 身分時，保留「ad-hoc／僅本機工程驗證」結論，停止正式發行路徑，不自動建立／匯入憑證、重簽使用中的 App 或上傳 Apple。

正式對外發行須另行完成有效 Developer ID、所需 hardened runtime／entitlements、內含程式由內而外簽署、公證結果及適用的 stapling／Gatekeeper 驗證，並由權利人確認授權範圍及通過實機驗收。Apple 的 [Developer ID 說明](https://developer.apple.com/developer-id/)涵蓋簽章、hardened runtime 與公證流程；本機預檢通過不替代上述步驟。

## 指定路徑的升版 TCC 驗收（另行執行）

這是需要使用者參與的驗收項目，不是上述離線命令的一部分，也不是本文件對硬體操作的授權。

1. 記錄舊版與新版精確 bundle 路徑、版本、bundle ID、簽章身分／designated requirement、GUI／CLI hash 及 macOS 版本。保留可回復的舊版副本，不從 App 名稱判斷實際執行版本。
2. 確認無使用者串流或其他裝置工作後，依約定安裝位置驗收更新；另建路徑的候選不能代替原位置升版驗收。以精確路徑開啟，核對實際 process executable path。
3. 用桌面預覽確認該 App 的擷取權限。若出現系統授權／重新開啟提示，保存具體狀態，交使用者處理後正常重啟同一確切路徑；不重設 TCC、不切換無法辨識的同名項目。
4. 記錄更新前後是否出現新提示、重新啟動後是否可預覽；即使 bundle ID 相同、Developer ID 相同，也不預先保證授權保留。CLI 是另一 executable，其擷取權限另測。
5. 桌面預覽成功僅代表擷取路徑可用，不等於 USB 握手、長時間串流、素材 Flash 或 LCD 顯示驗收。實機另按交接規則取得唯一擁有權和共用 guard。

## 診斷可靠性與資源驗收範圍

離線 `test_diagnostic_log` 使用可控 streambuf，驗證寫入／flush 失敗與真實有界緩衝溢位的計數及完整性標記。正常 Controller trace 回歸則核對 schema、兩次握手 context 與正常結束的 `log,end`。兩者通過仍不代表已測 Controller 在診斷磁碟故障下持續串流，也不代表真實磁碟寫滿或長時間硬體 soak 已通過；這些是獨立待驗收項目，不為測試結案放寬產品規則。

離線資源基線可使用 fake Source／Transport，以固定工作量比較 trace 關閉、開啟與故障情境的 RSS、CPU 時間、執行緒／FD 數及輸出大小，區分 warm-up 與穩態。C++ allocation 累計 bytes 不是 RSS；短測或 sanitizer 通過也不是無洩漏的長期證據。實機 soak 還需涵蓋 ScreenCaptureKit、USB 背壓、ACK／恢復與畫面時效，另外約定時長、基線與停止條件。
