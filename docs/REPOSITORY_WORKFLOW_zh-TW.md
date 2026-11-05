# 儲存庫工作流程

遠端 `main` 保存可重建的來源、測試與文件。[CONTRIBUTING](../CONTRIBUTING.md)說明修改與提交規則；換人或換機使用[交接入口](HANDOFF_zh-TW.md)，目前功能及問題以[現況](STATUS_zh-TW.md)為準。

## 來源與原始證據

`results/`、`dist/`、App、建置物件、下載的參考 PDF、外部量測及生成的設計圖維持本機保存，不隨一般來源提交。被忽略不代表可以刪除。需要證據時另外交接指定目錄與雜湊；文件摘要無法取代原始量測。

歷史 `MANIFEST.sha256`、`CHECKSUMS.sha256`、`FILE_INDEX.json` 及 `SOURCE_BASE.json` 描述各自建立時的包或來源，不是現行 checkout 的完整性證明。用實際 commit 與當次產生的清單／雜湊核對驗證來源。

## 文件維護

- README 只提供產品與建置入口；現況、待辦與證據基線集中在 `STATUS_zh-TW.md`，交接步驟集中在 `HANDOFF_zh-TW.md`。
- 現行文件連結使用可從 clone 取得的相對路徑。`scripts/check_docs.py` 檢查 Git 可見 Markdown 的本機檔案連結；不檢查網路連線、標題 anchor 或程式碼區塊。
- `docs/history/` 保留歷史原文，不由連結檢查器重新解釋當時的路徑。新的結論更新現況，必要時新增有日期的證據摘要，不改寫舊量測。
- 移動文件要同步更新現行索引；移動程式或工具前先確認建置與固定路徑依賴。

## CI 設定

[macOS workflow](../.github/workflows/macos.yml)檢查文件、shell 語法、App／CLI、離線套件、ASan／UBSan 及硬體 driver 連結。它不執行 driver、不擷取桌面、不寫素材 Flash，也不全面驗證歷史 runner、CMake 或發行包。

workflow 只有內容讀取權限，checkout 不保留憑證，Action 使用完整 commit SHA。更新時依[checkout 官方說明](https://github.com/actions/checkout/tree/v7.0.1)核對 runner／runtime 相容性；Dependabot 每月提出 Actions 更新，仍需審閱及 CI 驗證。相同分支的較新工作會取代較舊 CI，不能把被取消的 run 當成驗證通過。

所有建置使用隔離目錄；最終驗證須對應預定推送的來源，並檢查 run 的 head commit。CI 通過是當次環境下的證據，不等於實機或正式發行驗收。
