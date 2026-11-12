# MSU2 Mini project instructions

## Collaboration roles

The user's current instructions override this file, including role and model
choices; live terminal or session IDs and the CLI's current model are not
recorded here. Do not change another agent's model on your own. A multi-agent
team may use these four roles; a solo contributor can skip the role list.

- PM: scope, priority, coordination, final reports, relaying questions and
  approvals to the user. Does not read product code or diffs.
- DEV: implementation, tests, hard debugging, technical decisions; opens PRs.
  Merge, tag and release only with the user's explicit approval for that object.
- QA1: fixed test steps, CI and status checks, scheduled checks, simple file
  and diff checks, post-release verification.
- QA2: first-pass CI failure triage, screenshot/visual acceptance, comparing
  external feedback with code behaviour, research. Does not change product code
  or merge.
- Only the designated hardware tester may open the device; PM, DEV, and
  analysis roles do not access it.
- Keep each edit scoped to an agreed owner. Refactors must preserve protocol,
  ACK acceptance, timeouts, and display scheduling unless explicitly in scope.
- Start with README.md, docs/HANDOFF_zh-TW.md and docs/STATUS_zh-TW.md;
  use docs/ARCHITECTURE_zh-TW.md and CONTRIBUTING.md for implementation work. Historical
  handoffs contain local paths and old state; check dates and current evidence.
- Keep experiments in project-local results/ with isolated BUILD_DIR values.
  Do not overwrite a frozen candidate or the user's authorized App. Raw evidence
  and generated apps are not part of the source repository.

## Team messaging (optional, local tooling)

Applies only to a team that has the local Herdr `hc` and `hcmsg` tools; neither
is a prerequisite for contributing. In Herdr, `hc ls` shows the live terminals
and their groups; check the current group and IDs there instead of assuming
earlier ones. Members message each other with
`hcmsg <recipient> "<flag>" "<content>"`; the tool adds the header, sequence
number and time. Flags request a reply: `CBK` (report when done, failed or
blocked), `ACK` (reply on receipt), or none (notification). A reply carries
`RE <original ID> <OK|FAIL|BLOCK|ACK>` and no request flag. Start a task
message with its task code and a full-width colon; results point `RE` at the
original assignment. One topic per message, about 600 characters at most; put
longer content in a file and send the path. Merge, release, deletion, pushing
and settings changes need the user's approval for that specific object, valid
once and re-checked if the object (e.g. the commit) changes.

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
