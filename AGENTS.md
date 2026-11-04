# MSU2 Mini project instructions

## Active collaboration (user-confirmed roles, 2026-11-05)

- p1 is the PM: scope, coordination, review, and final reports. p2 owns tests,
  builds, and hardware. p3 (Claude Opus) owns difficult analysis and approved
  product changes; p3 must not access the device. Discover current pane IDs
  instead of assuming historical IDs still apply.
- The user's latest role and model choices override the historical model
  defaults below. Do not change another agent's model on your own. When the
  three-party workflow is active, use p2 for verification and p3 for analysis.
- Keep each edit scoped to an agreed owner. Refactors must preserve protocol,
  ACK acceptance, timeouts, and display scheduling unless explicitly in scope.
- Prefer README.md, docs/ARCHITECTURE_zh-TW.md and
  docs/REPOSITORY_WORKFLOW_zh-TW.md for source checkout orientation. Historical
  handoffs contain local paths and old state; check dates and current evidence.
- Keep experiments in project-local results/ with isolated BUILD_DIR values.
  Do not overwrite a frozen candidate or the user's authorized App. Raw evidence
  and generated apps are not part of the source repository.

## Historical default model routing and hardware invariants

- Physical hardware testing and verification must be delegated to a subagent
  using `gpt-6-luna` with reasoning effort `medium` (latest user preference: GPT-6 Luna 中,
  replacing the earlier high-effort settings).
  Spawn with `fork_turns: none` and supply the needed project context explicitly.
  The primary agent reviews the evidence and reports findings and recommendations.
- Routine test-results analysis must be delegated to a subagent using
  `gpt-6.1-sol` with reasoning effort `xhigh`, unless a specific rule applies.
- Analysis of unexplained problems after testing must use `gpt-6-astra` with
  reasoning effort `ultra`; if remaining Usage is below 25%, use `max` instead.
  Check current usage before choosing the effort for that analysis.
- After a hardware test task is complete, delegate its comprehensive analysis to
  `gpt-6-astra` with reasoning effort `ultra`, using `fork_turns: none` and an
  explicit evidence handoff. For unexplained problems, apply the usage-dependent
  `max` exception above. Analysis agents must not access the hardware.
- Only one agent may own the MSU2 serial port at a time. Coordinate device access
  and retain commands, exit status, CSV/logs, and executable hashes for experiments.
- Before each physical run, check both cu/tty aliases for existing owners and use
  the shared serial guard. If another test owns the device, wait for its natural
  completion; never interrupt it or launch a competing hardware test.
- Current scope (user update 2026-10-11) is the native macOS C++ application:
  external image-asset Flash writing, desktop mirroring, and stream management.
  The implementation lives in msu2_macos_app and reuses msu2_linux_v0_3 core.
  Preserve the existing display update algorithm; distinguish handshake failures,
  ACK timeouts, and dropped source frames. The earlier 45..60 FPS sweep is prior
  evidence, not a prerequisite for application work or a promise of 60 FPS.
  MCU firmware flashing is separate from external asset Flash and is not implemented.
  Windows x64 and Rust development remain deferred.
- Freeze or copy each executable before hardware experiments. Do not rebuild a
  binary while a hardware agent is using it; preserve before/after SHA256 hashes.
