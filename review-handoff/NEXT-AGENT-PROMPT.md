# Delegation prompt for the next orchestrator agent

You are taking over an in-progress full-codebase review and fix campaign for **Inventatory**
(C++17 terminal inventory app, Windows + Linux, repo `Kwiatens/Inventatory-Software`). The previous
orchestrator ran out of session credits. Read this file completely, then `AGENTS.md` in the repo root, then
`review-handoff/review/TRIAGE.md` (single source of truth; its last section is the status at handover) and
`review-handoff/tools/README-ENV.md`. Everything you need is in this branch's `review-handoff/` folder:
`review/` (findings per slice, plan, reviewer protocol, coverage data), `tools/` (build/test helpers, the shared
agent brief `AGENT-COMMON.md`), `gemini-prompts/` (prompts the user ran in a separate GUI), `branch-notes/`.

## 0. The user's standing rules (non-negotiable)
1. **At most TWO Claude subagents running at once** (Sonnet, `subagent_type: general-purpose`, `model: sonnet`,
   `isolation: worktree`, `run_in_background: true`). Cheap mechanical work (builds, greps, merges, CI reads)
   you do yourself. Gemini agents run on the USER's machine, never launched from here; if useful you may write
   exact low-risk prompts for them (see gemini-prompts/ for the style: exact code, tests-only or tiny edits).
2. **Commit style** (AGENTS.md): `type(scope): imperative lowercase subject`, <=72 chars, one logical change per
   commit, **TITLE ONLY, no body**, followed by exactly these trailers:
   ```
   Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
   Co-Authored-By: Paweł Kwiatkowski <110034652+Kwiatens@users.noreply.github.com>
   Claude-Session: https://claude.ai/code/session_01GpzAJJmnShUCNX4H69KjYW
   ```
   (Use your own session link / model name for the trailer if they differ.) No secrets, local paths or tool
   names in commit messages. Do not edit CHANGELOG.md (no version bump). Never create a pull request unless the user asks.
3. **Push policy**: the user authorised pushing finished, reviewed work **directly to `main`**, but only after the
   full Linux build + tests (Debug and Release) pass on the exact tree you push. Never push a red tree. Do not
   touch the user's uncommitted work if any appears. Windows-only code you cannot compile: keep changes minimal
   and let CI verify (see 2 below).
4. Work autonomously and report concisely. Keep a running record in `review-handoff/review/TRIAGE.md` if you
   can (it lives on this branch only; do not push it to main).

## 1. How verification works here
- Linux: `tools/fulltest.sh <srcdir> <name> [Debug|Release]` builds the real project (stub SQLite + system
  libsqlite3 + version shim; recipe in tools/README-ENV.md) and runs `inventatory_tests` and
  `inventatory_input_tests` with an unlocked keyring and strict locale mode. Run both Debug and Release on the
  integrated tree before pushing.
- Windows + Linux CI: `.github/workflows/ci.yml` runs on every push to `main`. Read results with the GitHub MCP
  tools (`mcp__github__actions_list` list_workflow_runs/list_workflow_jobs, `mcp__github__get_job_logs`
  with the failed job id). The Windows job is the only way to verify Windows code. Its test run stops at the
  first failed assert; fix forward and iterate. Linux CI has been green with every push so far.
- **First action**: read CI run `37430089428` (commit 32f5260 on main, "skip the case-variant restore conflict on
  Windows"). Its previous run failed only because a T10 restore test case created `Inventory.DB` next to
  `inventory.db`, which are the same file on Windows. If the Windows job fails again on another assert, it is
  almost certainly the same kind of case-insensitive/path-separator/permissions assumption in a test: fix the
  TEST (or, if the product code is wrong on Windows, the code), push, re-check. Main must end green on both jobs.

## 2. What is already done
Everything listed under "STATUS AT HANDOVER" in `review/TRIAGE.md`: ~30 fixes on `main`, built and tested.
Do not redo them. Important: `main` HEAD at handover = 32f5260.

## 3. What to do, in order
### A. Integrate the two handover branches (the biggest pending value)
Branches (pushed to origin, based on main 4d8e4de): `claude/handover-domain-agent` (24 commits) and
`claude/handover-ui-agent` (12 commits + 1 unfinished handover commit). Commit lists are in `branch-notes/`.
Procedure that worked: create a fresh worktree from `origin/main`, `git cherry-pick` the commits in order
(`git rev-list --reverse 4d8e4de..<branch>`), resolve conflicts, then run `fulltest.sh` Debug and Release, then
`git push origin HEAD:main` (fast-forward; re-fetch and rebase first if main moved). Known conflict hotspots:
`tests/inventatory_tests.cpp` (new tests are inserted just before `int main()` and the calls just before the
final `cout << "Inventatory core tests passed"`: when both sides add there keep BOTH, and remember to close the
earlier function with `}`), `src/App.h`, `CMakeLists.txt` source lists, `docs/`. Check for leftover conflict
markers with `git grep -n '^<<<<<<<\|^>>>>>>>'` before every commit.
- Domain branch: the agent was stopped before its final report; all 24 commits were committed but the combined
  tree was never built here. Expect to fix small compile/test issues. Integrate it first.
- UI branch: the 12 real commits are probably fine; the last handover commit
  (`chore(ui): save unfinished UI work for handover`, files `BomProjectPageRender.cpp`, `RackManagementPage.cpp`,
  `StockPage.cpp`, `AppUiItemDetails.cpp`, `AppUiShared.{h,cpp}`, tests) was written mid-task and never built:
  review it; finish it (likely UTF-8 display-width helpers and find-in-racks clipping / scrolling from findings
  J S3), or drop it. UI changes cannot be exercised interactively: prefer pure helpers with unit tests, and
  check keyboard/mouse parity per AGENTS.md. After integrating, give the maintainer a short manual UI checklist.
- You may run integration of both branches yourself and use your two agent slots for B/C below meanwhile.

### B. Work nobody has done yet (assign to at most two agents at a time; use tools/AGENT-COMMON.md as the shared
brief, which already contains the commit protocol, the build helper and the report format)
1. **Windows cluster** (verify only through CI): triage T13/T24/T25 (ANSI vs UTF-8 paths: `Environment.cpp`
   uses `_dupenv_s`; `Console.cpp` uses `GetOpenFileNameA/GetSaveFileNameA/SHBrowseForFolderA`; add a `/utf-8`
   MSVC flag and a UTF-8 application manifest; `path::string()` throws on non-ACP paths), T27 (Windows listener
   uses SO_REUSEADDR; use SO_EXCLUSIVEADDRUSE), T28 (hide-to-tray depends on GetConsoleWindow, wrong under Windows
   Terminal; plausible, design choice), and the 16 S3 items in `findings/W-windows-platform.md` (WinHTTP proxy
   config, clipboard history exclusion, bind address from gethostname, tray quit relaunch, console close handler,
   forceStop kills any same-path process, installer/uninstaller leftovers, Windows error text encoding, sharing
   violation retry, relative data dir accepted on Windows, accept loop spin). Verify each by reading first.
2. **Tests/build/CI** (`findings/K-tests-build-ci.md` S3 list): sanitizer/Clang/-Werror CI jobs, `/utf-8`
   reaches the app only by accident (overlaps 1), built binaries never launched in CI (add a smoke run),
   installer test uses a synthetic tarball and the `--update` path is untested, release publication robustness
   and job timeouts, no provenance/signing (report only), test fixtures use fixed `/tmp` names that collide when
   two runs overlap (use unique dirs and clean up), updater download/launch has no test seam, missing tests for
   Linux DigiKey transport and response parsers, scanner HTTP/credential tests (partly done by the user's Gemini
   commits c2073df and 2e8b2b0). Pair every change with CI verification.
3. **Remaining findings skipped or deferred on purpose** (decide, then do or document):
   - CE S3 "missing replay-state file is fail-open" (needs a design for a recovery UI: see the scanner
     agent's reasoning in TRIAGE/branch history, commit 4e0a602 and docs/scanner-transport-security.md);
   - CE S3 "authenticated rejected requests do not consume the counter" (analysis only, documented);
   - T26 BLE provisioning runs synchronously on the UI thread on both platforms (needs a deferred-apply
     path and hardware to verify: only do it if you can keep the provisioning transactional);
   - AB S3 BlueZ GATT characteristic lookup accepts characteristics from other cached devices
     (`src/platform/scanner/BleProvisioningServiceLinux.cpp` ~200-215), "interface follow / docker bridges";
   - leftovers reported by the platform agent: stale `Inventatory-update-*` folders after an abandoned download
     (add a startup sweep), the Linux update installer's `readlink -f /proc/<pid>/exe` keeps ` (deleted)`,
     `systemctl disable --now` still runs on the UI thread (bounded 25 s), Windows installer does not delete the
     update download folder, `execvp` between fork and exec in `ChildProcess.cpp` is not strictly async-signal-safe;
   - leftovers reported by the scanner agent: remaining `hasStoredDigiKeySecret_ = read().has_value()` sites
     (the domain branch may have fixed them), quick-label save failure in the data-folder-change path still
     discards the draft, dead code `App::enqueueDeviceQuantity` (domain branch may remove it);
   - all S4 quality items in each findings file (unreviewed, low priority; do opportunistically).
4. T16 (snapshot-per-commit history growth) beyond the streaming-validation change in the domain branch is a
   design topic: report, do not redesign.

### C. Wave 5 (the user asked for it to finish the whole review)
After A and B: (1) verify any remaining unverified S1/S2 and plausible items (T28, T36) as far as possible;
(2) produce one consolidated final report for the user: what was found (counts by severity from the findings
files), what is fixed (commit table), what was deliberately skipped and why, what could only be verified by CI or
not at all, and a short manual test checklist for things that need hardware or an interactive terminal
(scanner events during edit/import/failed save; restore into a folder with user files; mDNS on a real LAN with
`avahi-browse -rt _inventatory._tcp`; background mode off then second launch; UI actions control, scrolling
sheet, Enter after click). Put it in the session's primary working directory or publish it as an artifact; give
the user a short chat summary.

## 4. Practical tips from the previous run
- Agents sometimes report a false claim (one said Release asserts are compiled out: not true, the test file
  redefines `assert` as always-on). Read diffs yourself; run the real tests; trust CI for Windows.
- A safety check once blocked a `bash -c "...rm..."`-style one-liner; put multi-step shell in a script file.
- Parallel full builds on 4 cores are slow; do not run more than two builds at once; delete old build dirs.
- The tests write fixed `/tmp` names, so concurrent runs from different worktrees can flake: rerun once before
  investigating a lone failure in the restore crash-recovery test or the keyring step.
- Pushing to `main` triggers CI (about 7 minutes). Always read the result.
