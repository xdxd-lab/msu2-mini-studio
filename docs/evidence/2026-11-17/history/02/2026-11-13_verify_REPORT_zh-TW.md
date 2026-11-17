# MSU2-02 離線驗證補充（2026-11-13）

## 候選完整性與執行

- 冻結來源：`results/macos/msu2_02_20261112/source`；核對 `manifest.json` 235 個檔案，雜湊不符 0。
- 完整 suite 命令：`SANITIZERS=1 BUILD_DIR="$PWD/results/macos/msu2_02_verify_20261113/full_sanitized" CXX=clang++ bash msu2_macos_app/scripts/test.sh`，cwd 為凍結 source；300 秒 watchdog，exit 0，耗時 76.167 秒。
- 完整 stdout/stderr、含實際命令、exit 與耗時的 JSON：`full_sanitized.stdout.log`、`full_sanitized.stderr.log`、`full_sanitized.result.json`。stderr 空。suite 覆蓋 metadata unit、Controller trace、platform smoke、protocol/delta/scheduler 等；未 build GUI App。
- `test.sh` 含 `test_source_metadata.cpp`，不含 `test_controller_metadata_positive.cpp`。後者已有凍結候選記錄的 targeted normal 與 ASan/UBSan compile/run exit 0，詳 `results/macos/msu2_02_20261112/verification/positive/final/`；本次未重跑。
- 核對 SHA256：`controller.cpp` 2597b2498c0135b0efdcfcc41179a21019f5b732f32bfe36b78356fc80da8847；positive test bc4774ea81a49ca690120661c9ed5311997b6fa26d4217d2bf97739f0f0b446d；`test.sh` 5a4fb31fd74e67959fbbcb84d3b9f89f9ca6375051e0de01cbcf693812c7d144。

## CSV 相容性觀察

- 舊實機 trace `results/macos/stream_trace_20261105/diagnostics.csv` 有 472 列；候選正向 fixture `results/macos/msu2_02_20261112/verification/positive/final/normal/logs/20261112T141108_<PID>_1/diagnostics.csv` 有 64 列。兩者標頭完全相同，15 欄：`t_us,kind,stage,result,frame,patch,v0,v1,v2,v3,v4,v5,rx_len,rx_hex,detail`。標準 Python `csv.DictReader` 可完整解析，無欄位數/缺值問題。
- 新增 stage 為 `capture/identity`、`capture/pts_opaque`、`frame/callback_to_host`。欄位 schema 保持相容；依 stage 白名單拒絕未知列的外部 reader 仍可能需更新。repo 中未找到此 diagnostics schema 的生產端下游 parser；內建 Controller trace test 僅鎖 header、保留行欄位且不拒絕額外 stage。不能據此保證未知的外部嚴格 parser。

## 邊界

此輪只做離線 sanitizer/CSV 驗證，沒有 SCK callback、桌面權限、USB/serial 裝置或 LCD。fake source 的正向整合證據不能證明真實 ScreenCapture metadata 原子發布、實機效能成本、PTS 對應內容年齡或 LCD 顯示時間。
