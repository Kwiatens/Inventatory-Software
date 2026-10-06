# Shared brief for fix agents (read fully before starting)

You work in your own isolated git worktree (your cwd) of Inventatory, a C++17 terminal inventory app
(Windows + Linux). Read AGENTS.md in the repo root first and follow it strictly. The orchestrator
integrates your commits; other agents work in parallel on other areas, so stay inside your assigned scope.

## Source of truth for the issues
Review findings live in /tmp/claude-0/-home-user-Inventatory-Software/12d62aab-f424-56dd-b06c-ff8666ba88ef/scratchpad/review/inventatory-review/findings/*.md (one file per slice) and the
verified triage list in /tmp/claude-0/-home-user-Inventatory-Software/12d62aab-f424-56dd-b06c-ff8666ba88ef/scratchpad/review/inventatory-review/TRIAGE.md. Each finding has location, failure scenario,
evidence, proposed test and fix sketch. Findings were produced by static review and are not guaranteed:
VERIFY EACH ONE BY READING THE CURRENT CODE (main has changed since the review; some items are already
fixed, e.g. T1-T5, T8, T10, T14, T15, T17, T20, T21, T22, T32, T34) before changing anything. If an item is
not real, already fixed, or the right fix is a design decision beyond a small coherent change, skip it
and say so in your final report with the reason. Prefer the smallest coherent change per issue.
Never weaken a security/persistence/UI invariant from AGENTS.md to make a fix easier.

## Build and test (the real binary CAN be built here)
sqlite.org is blocked in this sandbox, so use the helper that wires stub SQLite + local FTXUI and runs the
real tests with an unlocked keyring and strict locale mode:
    /tmp/claude-0/-home-user-Inventatory-Software/12d62aab-f424-56dd-b06c-ff8666ba88ef/scratchpad/fulltest.sh <your-worktree-dir> <short-unique-name> [Debug|Release]
It configures once, builds inventatory + inventatory_tests + inventatory_input_tests, runs them and prints
"TESTS PASSED" or the failure. First build takes about 15-25 minutes (4 shared cores), later ones are
incremental, so batch your edits sensibly and do not rebuild after every tiny change. For a fast compile
check of a single file you may use g++ -std=c++17 -fsyntax-only -Wall -Wextra -I src (add pkg-config
flags for glib/gio/libsecret/libcurl as needed). The first test run right after a rebuild can fail once on a
keyring step ("Assertion failed: wrote"); the script retries once. Locale packages, libsqlite3-dev and all
build deps are already installed. Windows code cannot be compiled here: keep Windows changes minimal and
mechanical and say clearly in your report which Windows code is unverified.
Write new tests in tests/inventatory_tests.cpp (single assert-based program; its assert() is always-on),
test first (show RED on old code where practical), inside the existing style. Add a test for every changed
persistence/import/history/settings/printer/scanner behaviour. Keep scratch files in your scratchpad
directory, never in the repo. Do not commit build output.

## Commit protocol (IMPORTANT)
Make ONE LOCAL COMMIT PER LOGICAL FIX in your worktree as you go (after the build + tests for it pass, or
at least after the batch it belongs to passes). Do NOT push anything. Do not touch other worktrees.
Commit message = a TITLE ONLY (no body) following AGENTS.md: type(scope): imperative lowercase subject,
<= 72 chars, no trailing period, allowed types feat fix refactor perf style test docs build ci chore
revert, then a blank line and exactly these trailer lines and nothing else:
    Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
    Co-Authored-By: Paweł Kwiatkowski <110034652+Kwiatens@users.noreply.github.com>
    Claude-Session: https://claude.ai/code/session_01GpzAJJmnShUCNX4H69KjYW
Commit related test + doc changes together with their fix. Do not edit CHANGELOG.md (no version bump).
Never put secrets, local paths or tool/agent names in commit messages or code comments.

## Final report (reply with this, concise)
1. A table: issue id/title -> commit hash + title, or SKIPPED with reason (not real / already fixed / needs
   design / unverifiable).
2. Final full-test result (paste the "TESTS PASSED" line and the build config).
3. Anything you changed that touches Windows-only code or UI behaviour that could not be exercised.
4. New defects you noticed but did not fix (id-less one-liners).
