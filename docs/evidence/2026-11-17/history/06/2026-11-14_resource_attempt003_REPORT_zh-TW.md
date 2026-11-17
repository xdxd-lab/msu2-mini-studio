# MSU2-06 controller resource follow-up — attempt_003

## 執行摘要

按 PM 放行以既有 normal binary 執行唯一一次三段受控矩陣。PID <PID>，開始 `2026-11-15T01:13:16.722530Z`，結束 `2026-11-15T01:15:17.648092Z`，elapsed 120.924437 秒；exit 0、watchdog 150 秒未到期、runner error null。結束後 `os.kill(pid, 0)` 回 `ProcessLookupError`，確認 child 已退出。Binary 執行前後 SHA-256 相同：`067607009f470cdac4b9b97ce4ed820322682c8ae793d0c889e552897aabba1e`。

## 各階段觀測

| 階段 | sent 初次採樣 → 階段末 | 注入 | handshake / ACK timeout / transport error / reconnect | teardown 後 fake lease / transport / callback in-flight |
|---|---:|---:|---:|---:|
| normal | 3 → 1206 | 無 | 0 / 0 / 0 / 0 | 0 / 0 / 0 |
| write fault | 警告後 7 → 1210 | write failure 1 次 | 0 / 0 / 0 / 0 | 0 / 0 / 0 |
| flush fault | 警告後 7 → 1210 | flush failure 1 次 | 0 / 0 / 0 / 0 | 0 / 0 / 0 |

每階段約 40 秒；故障後 sent 均增加遠多於 3，且 probe 斷言 Controller 仍 busy、streaming、connected。兩 fault 的 `events.log` 各只有一條「診斷紀錄寫入失敗…串流繼續」警告，最終 `write_failed=1` 並註明 diagnostics 不完整；normal 的 `write_failed=0` 且含 end marker。三段 fake 資源於 Controller 析構後為零；source callback in-flight 為零，並通過額外 20 ms callback/write 計數不再增加檢查。`source_calls` 是 callback 呼叫總數（分別 1206、1210、1210），不是 source 物件數或全程資源量測。

## OS 資源採樣

每 5 秒一次，共 25 筆，覆蓋 elapsed 約 0–120.528 秒。第一次 `ps` 執行遇 `PermissionError(1, 'Operation not permitted')`；該採樣欄位記 unknown，並停用其後所有 `ps` 採樣，沒有重試或換工具。因此 RSS、CPU、thread 數均未知。`lsof` 25 筆都成功，FD 數觀測為 6–8；計數按 lsof 輸出列數估算，並含 process 開啟的輸出檔等描述符。這是短時段可見值，不代表無洩漏或長期穩定。

## 證據與範圍

- 完整正式命令：`COMMAND_FORMAL.txt`；啟動前 binary hash 與 watchdog 限制：`FORMAL_START_MANIFEST.txt`。
- Runner、PID/完整 argv/start/end/exit/hash/watchdog：`run_process.py`、`process_status.json`。
- Child stdout/stderr 與 25 筆 lsof/ps metrics：`process.stdout`、`process.stderr`、`process_metrics.jsonl`。
- 分段 events、stream CSV、注入 diagnostics：`evidence/mode0`、`evidence/mode1`、`evidence/mode2`。
- Runner 自身的短命 child 健全化測試與記錄：`PREP_zh-TW.md`、`self_test/`；沒有執行 USB、SCK、GUI 或 app，也沒有 ASan/UBSan。
- 所用 source 仍為 `de590ff05c6675406c031df1ba1fb0a48b848ab8`。本 attempt 只修改新 runner/evidence，不修改產品 source 或舊 attempt。故障 seam 是 test-only ostream adapter 加 fake source/lease/transport；不是實際檔案系統故障，不證明真實面板顯示或無限期無洩漏。
