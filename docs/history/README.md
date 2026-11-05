# 歷史調查與本機交接

這裡保存日期型調查報告的原文。2026-11-05 整理時只搬移、未改寫內容；文中的「目前」「未完成」、模型／pane／session、絕對路徑及命令均屬當時背景。現行結論請讀[現況與待辦](../STATUS_zh-TW.md)，接手工作請讀[交接入口](../HANDOFF_zh-TW.md)。

## 建議查閱順序

| 主題 | 報告 |
|---|---|
| 11/05 恢復與 Pair24 | [恢復工作紀錄](handoff/RESUME_AUDIT_20261105_zh-TW.md) |
| 10/31 Pair28、Raw 與診斷限制 | [串流結果](STREAM_DIAGNOSTIC_RESULTS_20261031_zh-TW.md)、[同機交接原文](handoff/README_交接入口_20261105.md) |
| 10/30 重連、同步及差分改善 | [品質審核](SOFTWARE_QUALITY_REVIEW_20261030_zh-TW.md)、[原計畫](SOFTWARE_QUALITY_PLAN_20261030_zh-TW.md) |
| 11/05 writer 模組整理 | [架構審核](ARCHITECTURE_REVIEW_20261105_zh-TW.md) |
| 速率研究 | [共用 FPS 分析](SHARED_FPS_ANALYSIS_20261015_zh-TW.md)、[早期掃描](FPS_SWEEP_20261010_zh-TW.txt) |
| 早期工具與環境 | [舊根目錄 README](README_legacy_zh-TW.txt)、[macOS 準備](MACOS_PREPARATION_RESULTS_20261010.txt) |

其餘檔案按主題及日期命名，包括握手、閒置、靜止轉換、外部研究與早期 UI。App 目錄也保留其既有日期型驗證文件；這些都不是新建置的驗收證明。

## 路徑沿革與證據

- 原根目錄 `docs/<日期型報告>` 現在是 `docs/history/<相同檔名>`。同一批報告之間的 Markdown 連結仍同層；原文中的純文字 `docs/...` 路徑須依此映射閱讀。
- 原 `README_zh-TW.txt` 搬至本目錄的 `README_legacy_zh-TW.txt`。
- 原 `handoff/` 的兩份紀錄搬至本目錄的 `handoff/`；舊交接入口另留短連結，避免既有使用方式失效。
- `results/`、凍結 App、下載文件及外部量測未上傳。Git 中的摘要、歷史 manifest 和來源沿革不是這些本機原始檔的替代品。

## 保留的歷史工具

`scripts/macos/`、`tests/macos/` 仍有固定路徑依賴 `msu2_cpp_v0_2` 相對符號連結及 `MSU2_MINI_CPP_v0_2_20261010/` 原始碼，因此本次沒有移動或刪除它們。`manual_test_kit/` 另依賴 v0.3 獨立 CLI。這些工具沒有由目前 App CI 全面驗證，不應當成現行 App 的 quick start。歷史來源與 runtime 授權原文保留不變。
