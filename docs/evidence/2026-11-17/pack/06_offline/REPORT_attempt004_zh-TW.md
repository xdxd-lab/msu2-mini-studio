# MSU2-06 attempt_004：e9c5f584 資源自採樣＋ASan/UBSan 單次執行（DEV）

## 設計（可重用）
- 來源：`../source` = e9c5f584（tree dd6b63c1）git archive；產品未改。test-only adapter 與 attempt_002 逐位元組相同（diagnostic_log.cpp 在 de590ff→e9c5f584 未變）。
- Probe `probe/test_controller_resource_v4.cpp`：沿用 attempt_002 的 fake lease/transport/source、3 段×40 s、全部原斷言；新增
  1) 程序內自採樣（不呼叫 ps/lsof）：task_vm_info phys_footprint/resident、task_threads、proc_pidinfo(PROC_PIDLISTFDS)；每 1 s＋各段 baseline/teardown，寫 `run/self_metrics.jsonl`。
  2) PR #4 新 diag 事件計數（capture/identity、capture/pts_opaque、frame/callback_to_host）與 log,end。
  3) 新斷言：teardown 後 2 s 內 threads、FD ≤ baseline；mode0 新事件 >0 且全為 unavailable（fake source 無 metadata）。
- `build_probe.sh normal|sanitized`（只編譯，0 warning）；`run_once.py`：唯一一次執行 sanitized 版，watchdog 180 s，輸出 exclusive-create。

## 結果（PID <PID>，120.55 s，exit 0，未逾時，stderr 0 B，binary 前後 SHA 102cf094… 相同，結束後 pid 不存在）
| 段 | sent | 握手/ACK逾時/傳輸/重連 | threads base→peak→post | FD base→peak→post | 新事件 identity/pts/cb2host（皆 unavailable） |
|---|---|---|---|---|---|
| normal | 3→1200 | 0/0/0/0 | 1→4→1 | 4→6→4 | 1201/1201/1201；log,end written=16625 lost_full=0 |
| write fault | 7→1200 | 0/0/0/0 | 1→4→1 | 4→6→4 | 0（寫入失敗後不落地；lost_after_failure=16612） |
| flush fault | 7→1200 | 0/0/0/0 | 1→4→1 | 4→6→4 | 6（失敗前批次；lost_after_failure=16625） |
- ASan/UBSan：全程無報告。fake 資源、callback in-flight 於 teardown 後皆 0。
- 新 diag 路徑：每次 producer 讀取 +2 事件、每次取幀 +1 事件，與 docs 一致；mode0 在 30 FPS×40 s 下 lost_full=0。

## 界線
- phys_footprint 在 ASan 下 22→155→282→326 MB 逐段上升且 teardown 後不回落；此為 ASan allocator/quarantine（預設保留已釋放記憶體、不還 OS）特性，第 3 段增量已收斂（+44 MB），**不能作洩漏或無洩漏判讀**。arm64 macOS 上 LSan 未啟用，無洩漏偵測。
- 正常版 footprint/RSS 仍未量；需另一次有界執行 normal binary（已建好，SHA cd174f98…），待 PM 放行。
- 仍為 fake transport/test-only ostream adapter；無 soak、無真實檔案系統故障、無實機。attempt_001–003 未重跑、未修改。
