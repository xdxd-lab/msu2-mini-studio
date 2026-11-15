# macOS 26 手動離線 CI

既有 `.github/workflows/macos.yml` 的 macos-15 push／PR 與手動流程保持不變。
新增 `.github/workflows/macos26-manual.yml` 僅接受 `workflow_dispatch`，使用標準
`macos-26` runner，不使用 larger runner 或建立自訂 runner。

GitHub 已公告 [macos-26 正式可用](https://github.blog/changelog/2026-02-26-macos-26-is-now-generally-available-for-github-hosted-runners/)；
標籤列表見 [官方 runner images](https://github.com/actions/runner-images)。
runner 映像會更新，標籤不是固定工具鏈快照。

合入預設分支後，可在 Actions 選擇 **macOS 26 manual offline verification**，
用 Run workflow 明確選擇待測 ref。此流程不因 push／PR 自動執行。
每次手動執行均應核對選取的 ref 與結果中的 commit SHA。

步驟沿用 macos-15：文件連結與 shell 語法檢查、App／CLI 建置、一般離線測試、
ASan／UBSan，以及 hardware driver 僅連結。維持 contents: read、固定 checkout SHA、
persist-credentials: false、15 分鐘 job 上限及相同 workflow/ref 的取消策略。
兩個 workflow 名稱不同，彼此不取消。

這不啟動 App／GUI、不擷取桌面、不連硬體、不執行 Flash、不發布或上傳產物，
也不改部署最低 macOS 版本。離線通過不能取代 SCK 權限、LCD 或 USB 實機驗收。
本候選只經本地靜態檢查；macOS 26 遠端建置與測試須待獲准推送後實際執行。
