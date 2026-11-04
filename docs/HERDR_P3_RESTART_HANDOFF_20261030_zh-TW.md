# wF:p3 重啟交接（2026-10-30）

Claude CLI 更新重啟前的上下文摘要。原 session ID：`9ec11df2-05e3-4b65-ba48-84db340dd5be`，由 p1 在原 pane `wF:p3` 以同一 ID 恢復。詳細依據見 [品質計畫](SOFTWARE_QUALITY_PLAN_20261030_zh-TW.md) 與 [審核結果](SOFTWARE_QUALITY_REVIEW_20261030_zh-TW.md)。

## 角色分工（本輪例外，優先於歷史文件）

- **wF:p1 ChatGPT Codex**：統籌、計畫、程式審核、正式報告。
- **wF:p2 ChatGPT GPT-6.1 Terra／中**：測試、隔離建置、簡單操作，是唯一可以持有序列埠的 agent。
- **wF:p3 Claude Opus（本 pane）**：困難分析和經核准的產品程式修改。測試、腳本及文件分別由 p2 和 p1 負責。回覆一律在本 pane，除非另有指示。

## 已完成

產品程式修正共 4 項，已凍結，並通過離線驗證，p1 判定接受。

- `msu2_macos_app/src/controller.cpp`
  - 新增 `Impl::set_flags`：在 condition variable 使用的同一把 mutex 內改旗標，解鎖後才 `notify_all`，避免漏掉喚醒。`~Impl`、`disconnect`、`stop` 都改用它。
  - 閒置重握手失敗會寫一列 `idle_renewal_failure` CSV，並遵守 `auto_reconnect` 設定，不算進 ACK 或傳輸失敗。重連成功後完整重建狀態；握手耗時不計入 encode_ms。
  - `recovery_attempts` 只在一次邏輯更新真正送出後才歸零。
- `msu2_linux_v0_3/src/delta.cpp`
  - 新增固定容量的 `EncodedGeometries`，跳過重複的矩形序列，不再重複編碼。
  - 全幅矩形直接編碼輸入像素，不先複製裁切。
- 驗證（p2 執行）
  - 凍結候選的 normal、ASan/UBSan、build、codesign 都 exit 0。
  - 舊版在 `idle-recovery` 和 `separated-recoveries` 會失敗，新版通過。
  - 修改前後共 10 份 `.plans.bin` 逐 byte 一致。
- 效能（每次 plan 的經過時間，不是 CPU time）
  - full／noise 的 Raw 和 Pair 都降到約 0.30–0.36 倍，五個樣本的範圍不重疊；new 次數 19→9，申請 bytes 約少 84%。
  - 局部、稀疏、對齊矩形的時間差異看不出來，配置量略減。
- 證據根目錄：`results/macos/software_quality_20261030/`。p3 的原始分析在 `opus_review_terminal.txt`。

## 不可宣稱或未完成

- 沒有實機、USB、LCD FPS 量測，也沒有 RSS、峰值記憶體或洩漏量測。macOS 上沒有 LSan。
- 漏喚醒是依程式機制推理修正，沒有用測試重現。
- Pair 實機逾時缺少 trace，不可臆測已修好。
- 沒有找到 MCU 韌體 binary，也沒有 ACK 反組譯結果。韌體行為只能當作未證實的假說。
- 舊工作樹那次 sanitizer 逾時沒有對應的確切來源版本，不能算本候選的成功或失敗證據。
- 後續工作依品質計畫第二階段的順序：
  1. Pair 診斷
  2. 桌面時效指標
  3. 受控實機對照
  4. 分區公平性（center3/60 左區飢餓）
  5. 長時間 RSS 和資源測量
  6. 簽章、升版和授權驗收
- 尚無專門的斷言驗證幀的新鮮度。CMake/CTest 路徑因為環境沒有 cmake，沒有執行。

## App 路徑

- **候選驗收版**：`results/macos/software_quality_20261030/reviewed_build/MSU2 Mini Studio.app`
  - App 執行檔 SHA256 `06b48a5e…8d22`，CLI SHA256 `311a47e1…caf0`。
  - ad-hoc 簽章，未驗證螢幕擷取授權。
- **正式版（未替換）**：`msu2_macos_app/build/MSU2 Mini Studio.app`
  - 執行檔 SHA256 `68c9f0d8…c16c135`。
- 不要用 App 名稱搜尋，在多份副本間挑選。p3 不啟動 App，不重設 TCC。

## 硬體鎖規則

- 同一時間只能有一個 agent 持有 MSU2 序列埠。本輪只有 p2 可以持有。p3 不碰硬體，也不跑測試。
- 每次實機執行前，都要檢查 cu/tty 兩個別名是否已被占用，並使用共用 serial guard。預設鎖位置是 `results/macos/.device-locks`。
- 若裝置已被其他測試占用，要等它自然結束，不可以中斷，也不可以另開競爭的硬體測試。
- 實機實驗前先凍結或複製執行檔，並保留命令、退出碼、CSV/log 和前後 SHA256。

## 修改邊界

- 保留傳輸協定、ACK 接受規則和顯示更新策略。
- 不修改 UI、正式 App、簽章或 TCC。
- 不做單純格式化或大幅架構改動，也不加沒有用途的抽象。
- 動手修改前，先由 p1 劃分檔案範圍並核准。
