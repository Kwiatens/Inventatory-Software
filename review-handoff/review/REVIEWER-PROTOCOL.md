# Reviewer protocol (read fully before starting)

You are one of two parallel code reviewers on Inventatory, an open-source C++17 terminal inventory app
(FTXUI, SQLite, Windows + Linux). Repo: /home/user/Inventatory-Software (HEAD b6c9be6 = main, includes the owner DigiKey-on-Linux commit; tree is clean)
Read AGENTS.md in the repo root first — its invariants (persistence/restore atomicity, secrets, Scan R1
security, UI-thread-only state mutation, keyboard/mouse/action-sheet parity) are review criteria.

HARD RULES
- READ-ONLY. Do not edit, create, or delete any file inside the repository. Do not run git commands that
  change state (no checkout/stash/commit/reset). Do not run cmake/ctest builds (another build is running).
  You may compile tiny standalone snippets in the scratchpad dir to check language/library behaviour.
- The owner DigiKey Linux change is now committed (b6c9be6); review it as normal code.
- Write your output ONLY to the findings file path given in your task.

WHAT TO HUNT (priority order)
1. Real defects: crashes, UB, data loss, corrupted persistence, security holes, races, resource leaks on
   error paths, wrong results, broken error recovery.
2. Linux-specific defects — most escaped bugs are on Linux. Known escaped bug classes; actively search
   for SIBLINGS of these in your slice:
   - locale-dependent parsing/formatting (std::stod/strtod/printf/iostream/tolower/isdigit/isspace with
     the global locale; isX(char) with negative chars = UB). A bug broke unit search on Linux.
   - files rewritten when unchanged / non-idempotent startup side effects (systemd unit, .desktop file).
   - process lifecycle: relaunch after update, background service takeover, forced stop, signals, zombies,
     fd inheritance (O_CLOEXEC), SIGPIPE.
   - platform asymmetry: #ifdef _WIN32 code whose Linux branch is missing/stubbed/weaker; Windows-only
     assumptions (\r\n, case-insensitive paths, backslashes, wide strings, %LOCALAPPDATA%).
   - Linux desktop/terminal identity issues.
3. Missing tests: behaviour in your slice that tests/inventatory_tests.cpp does not exercise, especially
   failure paths. grep the test file for the function names.
4. S4 quality (separate section, lower priority): dead code, duplicated logic, over-long code for the job,
   needless copies, O(n^2), misleading names/comments. Only things worth changing in an open-source
   showcase codebase; no formatting nits.

DISCIPLINE
- Every finding must cite path:line, a CONCRETE failure scenario, and code evidence. If you cannot describe
  inputs/state that produce a wrong outcome, it's not an S1–S3 finding.
- Trace callers before claiming a bug: check whether input is already validated upstream, whether the
  "missing" lock is held by the caller, whether the "leak" is freed by RAII elsewhere.
- Self-check each finding once before writing it: "what would the author say to refute this?"
- Prefer 8 solid findings over 30 speculative ones. Mark confidence honestly.

SEVERITY: S1 data loss/security/crash/corrupt persistence · S2 wrong behaviour in supported workflow,
Linux/Windows divergence, race · S3 edge case / poor error recovery · S4 quality/perf.

OUTPUT FILE FORMAT (markdown)
# Slice <ID>: <name>
## Coverage
Files read (all of them), anything skipped and why.
## Findings
### [S?] <title>
- Location: path:line
- Category: correctness|security|concurrency|persistence|linux-portability|ui-parity|perf|quality|test-gap
- Failure scenario: ...
- Evidence: (quote <=10 lines)
- Confidence: high|medium|low
- Proposed test: ...
- Proposed fix (sketch): ...
## Test gaps
Bullet list: untested behaviour -> suggested test case.
## Quality (S4)
Same finding format, briefer.

When done, reply with just: the file path and counts per severity.
