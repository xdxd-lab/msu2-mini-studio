# 架構與原始碼交接審核（2026-11-05）

p1 負責範圍、程式審核與儲存庫整理；p3 實作及複核；p2（本輪介面設定 GPT-6-Luna medium）執行軟體離線驗證。

## 修改

從 Controller 搬出診斷 writer，形成內部 `diagnostic_log.hpp/.cpp`。Controller 保留工作排程、連線、串流與素材操作協調；新模組依賴 `TraceSink` 及外部提供的牆鐘文字。原函式本體、成員順序、緩衝容量、寫檔策略及事件 schema 保留。建構時傳入牆鐘文字會比原本略早取得，秒的邊界可能不同；拆成不同編譯單元也可能改變內聯，未量測其效能影響。

補充 worker／producer／writer 的資料擁有權、鎖、狀態轉移、參考影格提交及時間定義。build/test 加入新物件；硬體 driver 的建置入口加上依賴並允許隔離 BUILD_DIR。沒有變更協定、ACK 接受、timeout、顯示排程或修正 Pair 逾時。

新增根 README、架構與 Git 工作流程文件；更新歷史文件的入口與適用日期。Git 排除結果、發行 App、建置物件、外部 PDF、外部歷史量測與生成的設計圖，保留原始碼、必要舊工具依賴、合成測試圖與既有授權原文。

## 驗證

從 Git 暫存區匯出獨立來源，使用 Apple clang 21.0.0、arm64 macOS；所有步驟均退出碼 0：

| 項目 | 結果 |
|---|---|
| 完整離線套件 | 通過，68.848 秒，含 controller/session trace 回歸與約 308 萬條 delta 斷言 |
| ASan／UBSan 全套 | 通過，77.209 秒；未見 sanitizer 診斷 |
| App／CLI 乾淨建置 | 通過，8.238 秒 |
| codesign deep/strict verify | 通過 |
| hardware driver | 連結成功，未執行 |

完整本機證據：`results/macos/architecture_cleanup_20261105/index_validation.log`、`index_source_sha256.txt`、`verification_plan.md` 及 `p3_review/`；這些是本機檔案，不隨 Git 原始碼推送。測試來源匯出後補充的 workflow 與文字文件經審閱，產品來源／測試／建置腳本保持同一版本。

加入 macOS GitHub Actions 流程，其指令對應本輪離線驗證；尚未在 GitHub runner 執行，macos-14 與本機工具鏈不同，不能將本機通過描述為遠端 CI 已通過。CMake、歷史 Python 診斷套件、GUI 互動及硬體不在本輪驗證範圍。寫檔失敗與緩衝溢位的安全注入亦未新增。

## 審核與限制

接受這批局部架構整理，現有離線回歸提供行為保持的證據。診斷開啟仍有鎖與寫檔成本；本輪沒有量測 FPS 或宣稱修復實機斷線。Pair24／28 的 ACK timeout 和分區公平性等未完成議題保留。

暫存內容已檢查常見金鑰格式、檔案大小及忽略規則，未發現候選金鑰；這不是完整的機密資訊鑑識。根目錄未新指定授權，沿用各子目錄既有 LICENSE。歷史交接包含本機路徑與代理 session 資訊，遠端可見性由使用者決定。
