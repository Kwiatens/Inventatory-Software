# Inventatory code review: orchestration handoff

You are taking over as **orchestrator** of an in-progress full-codebase review of Inventatory
(`/home/pawel/Dokumenty/Github/Inventatory-Software`, C++17 terminal inventory app, Windows + Linux).
Read this file fully, then `REVIEW-PLAN.md`, `TRIAGE.md` and the repo's `AGENTS.md` before doing anything.
All review artifacts live in this folder (`/home/pawel/Dokumenty/Github/inventatory-review/`), outside the repo on purpose.

Handoff date: 2026-10-05. Previous orchestrator: a Claude Code session that ran low on usage.

---

## 1. Hard constraints from the user (non-negotiable)

1. **At most TWO Claude subagents running at once.** The user is on a Pro plan. Twelve parallel agents were launched once and the
   user had to stop them. Work in waves of two. Do cheap mechanical checks (builds, greps, warnings) yourself.
2. **At most TWO Gemini runs at once**, through the Antigravity CLI, for **lower-risk** work only (Gemini is weaker):
   - Binary: `~/.local/bin/agy`. Model: `gemini-3.8-flash-high`.
   - **Subscription billing only, never API billing.** Auth is the OAuth token in `~/.gemini/antigravity-cli/`. Never set or
     export `GEMINI_API_KEY`, `GOOGLE_API_KEY` or Vertex variables. Check `env | grep -iE "GEMINI|GOOGLE_API|VERTEX"` is empty first.
   - The user's global Antigravity preset is TURBO with eager auto-execution. Never pass `--dangerously-skip-permissions`.
     Never edit `~/.gemini` settings or allow-lists without asking.
   - Read-only run: `agy -p "$(cat prompt)" --model gemini-3.8-flash-high --mode plan --sandbox --print-timeout 2900s > out.md`
   - Editing run: same command, but `--mode accept-edits`, run **only inside an isolated git worktree** (cwd = worktree).
   - **Headless mode auto-denies shell commands.** Gemini can only read and edit files. So run greps or scripts yourself
     and embed the results in the prompt (one argv argument is limited to about 128 KB). Tell it explicitly: "You cannot run shell
     commands; use only your file read/edit tools". It still sometimes tries a shell command and then exits with no output
     (check its `.err` file). Give Gemini very precise specs, ideally with the exact code to write. Always review its diff yourself.
3. **Ask before committing, pushing, or opening PRs.** No commit has been made yet.
4. The user's **uncommitted work in the main checkout** (`src/platform/digikey/*.cpp`, `tests/inventatory_tests.cpp`,
   removing `#ifdef _WIN32` guards to enable DigiKey on Linux) must never be modified, stashed or reset.
5. Follow `AGENTS.md`: commit convention `type(scope): imperative lowercase subject`, one logical change per commit, tests
   for every behavior change, never put secrets in logs or diffs. Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
6. Reviewer subagents: `subagent_type: general-purpose`, `model: sonnet`, `run_in_background: true`. Their prompt
   starts with "First read `<this folder>/REVIEWER-PROTOCOL.md` and follow it exactly." The protocol makes them read-only, requires
   evidence, a failure scenario, a proposed test, and severity S1–S4, and has them write to `findings/<slice>.md`. Tell each reviewer
   which findings are already known so it doesn't re-report them (see TRIAGE.md).

## 2. Process (from REVIEW-PLAN.md)

Baseline machine checks → slice review in waves of 2 → dedup/triage → independent verification (the orchestrator checks the code;
for important items, a skeptical verifier or a failing test) → user picks fixes → fixes test-first in worktrees →
the orchestrator reviews the diff and runs Release + ASan ctest → the user approves the commit.

## 3. Status

| Wave | Slice | Status | Output |
|---|---|---|---|
| 0 | Baseline: Clang ASan+UBSan Debug ctest (clean), Clang strict warnings | DONE | findings/0-orchestrator-baseline.md |
| 1 | A+B Linux platform (systemd, console, BlueZ, HTTP, mDNS, libsecret) | DONE, top items verified | findings/AB-linux-platform.md |
| 1 | I Shell/threading/settings/updater | DONE, top items verified | findings/I-shell-threading-update.md |
| 2 | C+E Scan R1 protocol + backup/restore | DONE, S1 verified | findings/CE-scan-backup.md |
| 2 | D+F Storage/history + inventory domain | DONE (file complete), **NOT yet verified** | findings/DF-storage-domain.md |
| G | Locale/ctype sweep (Gemini + orchestrator spot-check) | DONE: codebase clean apart from T13 | findings/GEM1-locale-sweep.md |
| G | Test coverage map (Gemini) | FAILED twice (Gemini tried shell). Folded into wave 4 K. Raw data: `coverage-raw.tsv` | — |
| 3 | G+H Imports/DigiKey (incl. the user's uncommitted Linux DigiKey change) + label printing (CUPS) | TODO | — |
| 3 | W Windows platform layer (read-only; cannot compile Windows here) | TODO | — |
| 4 | J UI pages + ActionRegistry (key/mouse/action-sheet parity, 100x30, UTF-8 width) | TODO | — |
| 4 | K Tests/build/CI (coverage map from coverage-raw.tsv, test quality, CI matrix, locale-matrix run) | TODO | — |
| 5 | Verification of all unverified S1/S2 (≤2 verifiers) + final report to the user | TODO | — |

Slice prompts used so far are reproduced in REVIEW-PLAN.md (focus columns). Reuse that style for waves 3–4.
**Wave 3 starts only when the user says so**, and the same goes for each later wave.

## 4. Fix in flight: UTF-8 text input (T1)

- Worktree: `/home/pawel/Dokumenty/Github/Inventatory-Software-utf8`, branch `fix/utf8-text-input`, based on HEAD `1c75564`
  (without the user's DigiKey changes). Created with `git worktree add`.
- Gemini implemented it from `prompts/gemini-utf8.prompt`. The previous orchestrator reviewed the full diff (9 files, +40/−18):
  `KeyEvent.text` field, `appendKeyText`/`eraseLastCharacter` helpers in `src/platform/system/Console.h`, translateEvent in
  `AppShell.cpp`, 9 free-text inputs switched over, history search now accepts non-ASCII, regression test added.
- **GCC Release build plus ctest pass** (`build-agent-release/` inside the worktree, gitignored).
- **COMMITTED** as 90d0dd7 on branch fix/utf8-text-input (not pushed, not merged). Commit message:
  `fix(ui): keep multi-byte UTF-8 characters intact in text input`. Also suggested to the user: a manual TUI check that typing
  Polish letters works in search, edit and settings fields (AGENTS.md UI verification).

## 5. Top verified findings (details in TRIAGE.md and findings/)

- **S1 T10**: Restore renames the whole data dir aside and `remove_all`s it, so non-allowlisted user files are permanently lost
  (InventoryRestore.cpp:155,173; InventoryTransferRecovery.cpp:157).
- **S2 T1**: typed multi-byte UTF-8 truncated (fix ready, see above).
- **S2 T2**: Linux mDNS never starts. `avahi-publish-service` has no `--interface` option (MdnsService.cpp:255-267). The fix must keep
  the private-LAN-only invariant (consider the Avahi D-Bus EntryGroup API with an interface index).
- **S2 T3**: Linux: disabling the background service in Settings releases the single-instance lock while the TUI keeps running
  (SettingsPageSave.cpp:290 → BackgroundControllerLinux.cpp:377). Windows stop() doesn't do this.
- **S2 T4**: Linux update download has a 15 s *total* timeout for an asset of up to 512 MB (UpdateService.cpp:846).
- **S2 T5**: no SOCK_CLOEXEC/accept4/pipe2. A browser started via xdg-open inherits the scanner listen socket.
- **S2 T8**: `tests/terminal_input_parser_tests.cpp` uses the standard `assert`, so it is a no-op in Release CI (NDEBUG).
- **S2 T11**: the HTTP worker reads `printerService_.configuredPrinter()` (a const string&) while the UI thread may assign it (string data race).
- S3s: T6 select() EINTR drops all pending clients; T7 CredentialStore::read conflates "keyring unavailable" with "missing",
  so the settings rollback can erase the real DigiKey secret; T13 Windows non-ASCII path case folding.
- Unverified S2s worth verifying first: T12 (device-event commit failure spins the UI loop and blocks later events), the six S2s in
  DF-storage-domain.md (scanner commit baseline after a failed save; lost update while the edit form is open; full snapshot per
  commit + full history load; "4u7"/"M3" misclassified as resistances; BOM false matches; scanner placeholders never get a slot).

## 6. Environment notes

- Build dirs (gitignored) in the main repo: `build-agent-clang/` (strict warnings), `build-agent-asan/` (Clang ASan+UBSan).
  GCC sanitizer runtime isn't installed, so use Clang for sanitizers:
  `-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"`.
- Faster configure in a new worktree (reuse fetched deps):
  `-DFETCHCONTENT_SOURCE_DIR_FTXUI=<main>/build-agent-clang/_deps/ftxui-src -DFETCHCONTENT_SOURCE_DIR_SQLITE3_AMALGAMATION=<main>/build-agent-clang/_deps/sqlite3_amalgamation-src`
- No clang-tidy, cppcheck or shellcheck installed. `avahi-publish-service` is installed (useful for verifying T2).
- Memory notes for this project live in `~/.claude/projects/-home-pawel-Dokumenty-Github-Inventatory-Software/memory/`.

## 7. Immediate next steps for you

1. Tell the user you've taken over and summarize status in 3–4 lines.
2. UTF-8 fix is committed (90d0dd7). Ask the user whether to merge it into main or open a PR.
3. Verify the D+F S2 findings yourself (read the cited lines). Add CONFIRMED/PLAUSIBLE/REFUTED rows to TRIAGE.md.
4. When the user says go, launch wave 3 (G+H, W): two Claude agents. Optionally use Gemini for a mechanical task
   (e.g. the coverage-map prioritization with `coverage-raw.tsv` embedded and file reads only).
5. Keep TRIAGE.md as the single source of truth. Update it after every verification.
