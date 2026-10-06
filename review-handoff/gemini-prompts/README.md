# Gemini tasks (low risk, mechanical). Run on your machine.

Prereqs (from the handoff): `env | grep -iE "GEMINI|GOOGLE_API|VERTEX"` must print nothing; never pass
--dangerously-skip-permissions. Max two Gemini runs at once. Each task gets its own worktree from main so
runs never touch your main checkout:

    cd ~/Dokumenty/Github/Inventatory-Software && git fetch origin
    git worktree add ../Inventatory-Software-g1 -b chore/input-test-assert origin/main
    git worktree add ../Inventatory-Software-g2 -b fix/cups-queue-status    origin/main
    git worktree add ../Inventatory-Software-g3 -b fix/update-download-timeout origin/main

Run (cwd = that worktree; the prompt files are in this folder):

    cd ../Inventatory-Software-g1
    agy -p "$(cat /path/to/g1-input-test-assert.prompt)" --model gemini-3.8-flash-high --mode accept-edits --sandbox --print-timeout 2900s > ../g1.out.md 2> ../g1.err

Then check `git diff` yourself, build and test (Release + ctest), and only then commit:

| Task | Fixes | Branch | Suggested commit |
|---|---|---|---|
| g1 | T8 input test is a no-op in Release | chore/input-test-assert | test(input): keep terminal input parser checks active in release builds |
| g2 | T21 + T22 CUPS queue status/locale | fix/cups-queue-status | fix(printer): detect disabled CUPS queues and parse lpstat in the C locale |
| g3 | T4 update download 15 s total timeout | fix/update-download-timeout | fix(update): abort stalled update downloads instead of a 15 s total timeout |

Expected verification: g1 - `ctest -R inventatory_input` must still pass; to prove it now works, temporarily flip one
expected value in the test and confirm Release fails. g2 - `ctest -R inventatory_core`; manual: `lpstat -p -d` on a
machine with CUPS (disable a queue with `cupsdisable NAME` and check Printer setup shows it not ready). g3 - build only
(no unit test possible); manual: throttle the download (`tc` or a slow mirror) past 15 s.
If Gemini exits with no output, check its .err file (it sometimes tries a shell command and quits); just rerun.

## Batch 2 (same workflow: one worktree per task from origin/main, accept the diff only after you read it)
| Task | Fixes | Branch | Suggested commit |
|---|---|---|---|
| g4 | T32 uiTargets_ vector -> deque (S1 memory safety) | fix/ui-targets-stable-refs | fix(ui): keep UI target bounds stable while the frame is laid out |
| g5 | T34 invisible Projects restock quantity prompt | fix/bom-restock-prompt | fix(project): show the received quantity prompt when restocking a BOM shortage |
| g6 | T30 locale tests can fail instead of skipping | test/locale-tests-strict | test(core): let CI require the comma-decimal locale regression tests |

## Batch 3
| Task | Fixes | Branch | Suggested commit |
|---|---|---|---|
| g7 | J S3: Enter on a blank DigiKey client secret in Settings overwrites the stored secret with an empty one | fix/digikey-empty-secret | fix(settings): reject an empty DigiKey client secret in the settings panel |
| g8 | J S3: non-ASCII-only parameter/category names match every lookup (UTF-8 sibling of T1) | fix/parameter-label-empty-key | fix(ui): stop non-ASCII parameter names from matching every lookup |
| g9 | K S3: AGENTS.md Windows build command misses inventatory_input_tests | docs/agents-windows-targets | docs(project): build the input test target in the Windows commands |

## Batch 4 (do these AFTER the current fixes are pushed to main: they all edit tests/inventatory_tests.cpp or CI)
| Task | What | Branch | Suggested commit |
|---|---|---|---|
| g10 | T-test: Scan R1 HTTP negative cases (404/400/413/401/409), tests only | test/scanner-http-negative-cases | test(scan): cover rejected requests and stale counters on the device endpoint |
| g11 | T-test: resolveWorkspaceScannerCredential + validScannerToken, tests only | test/scanner-credential-resolution | test(scan): cover scanner credential resolution |
| g12 | T29: .github/workflows/ci.yml for PRs and main | ci/pull-request-workflow | ci(release): build and test pull requests and main |
