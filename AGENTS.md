# Inventatory Agent Notes

## What This Software Is

Inventatory is a lightweight, keyboard-first Hardware Inventory Management System built in C++ as a terminal application.

The goals are:

- fast navigation with minimal clutter
- instant search and filtering
- a clear dashboard for system health and alerts
- a split-pane stock browser with item details on the same screen
- a local HTTP bridge that the Inventatory Scan hardware device talks to

## Main Behaviors

- The dashboard is the landing page.
- The stock browser shows part name, category, and quantity in the list, with full details for the selected item.
- Search should work by keyword, category, tag, parameter, location, SKU, status, and quantity filters.
- Item details should include the manufacturer part number, effective datasheet link, and IECD enrichment status.
- The Inventatory Scan hardware device pushes scans and status reports to the local bridge, which passes them back to the terminal app.

## Design Principles

- Prefer speed over visual complexity.
- Keep interaction keyboard-first.
- Keep the UI industrial, clean, and readable.
- Avoid adding unnecessary layers or frameworks.
- Favor clear data flow and simple state handling.
- Keep each page's render logic and key handling together under `src/ui/pages/`.
- Keep shared UI formatting and detail helpers under `src/ui/shared/`.
- Keep inventory implementation split across focused files under `src/core/`, with string/id helpers, query logic, serialization, history, storage, seed data, scan handling, and SQLite glue separated instead of recombined into one file.
- Keep startup/bootstrap path helpers in `src/app/AppBootstrap.cpp`, shared app workflows and state helpers in `src/app/AppActions.cpp`, and controller wiring in `src/app.cpp`.
- Keep the app controller split small and focused; move new cross-page helpers into `src/app/` instead of growing the controller file again.

## Security And Safety

- Never hardcode personal paths, usernames, machine names, local plugin folders, or user-specific fallback locations into public code.
- Never commit secrets, tokens, `.env` values, private keys, or credentials; load them from environment variables or local config files that stay out of git.
- Before committing or pushing, scan changed files for `C:\Users\`, home-directory paths, API keys, and other machine-specific references.
- If a change would expose private local data in public history, stop, report it, and rewrite the smallest possible history slice needed to remove it.
- Prefer runtime discovery and generic defaults over developer-machine fallbacks.
- Keep compatibility fallbacks only when they are truly user-data migration paths, and document why they exist.
- If a cleanup requires force-pushing rewritten history, preserve unrelated local work and verify the remote branch points only at the sanitized commit range.

## Code Structure

- `src/ui/pages/` owns page-local TUI rendering and page-specific keyboard handling.
- `src/ui/shared/` owns reusable terminal formatting and inventory detail helpers.
- `src/core/` owns inventory domain logic, with the implementation split across focused files for helpers, query, serialization, history, storage, seed data, scan handling, and SQLite support.
- `src/app/AppBootstrap.cpp` owns filesystem bootstrap helpers and database reuse paths.
- `src/app/AppActions.cpp` owns shared app state helpers, import workflow, scan handling, and IECD enrichment.
- `src/app.cpp` owns live terminal render/input wiring.

## Data And Files

- Inventory data is stored locally in `Documents/Inventatory/inventory.db`.
- Activity history is stored locally in `Documents/Inventatory/activity.tsv`.
- On first launch, the app copies the existing OG inventory database if it is available, so the inventory can be reused without rescanning.
- The app does not ship with built-in sample inventory; an empty store on first launch is expected until the user adds parts or imports data.

## Build And Run

Typical local workflow:

```powershell
cmake -S . -B build
cmake --build build
.\run.ps1
```

Tests:

```powershell
.\build\Debug\inventatory_tests.exe
```

After verification, please build the software - so the user can test it on their own:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

## Required Agent Workflow

After each finished change, always:

1. Build the software.
2. Execute the software in a user-owned new terminal window.
3. Verify the change behaves as intended.
4. If the change touches the TUI or any terminal rendering code, capture a screenshot of the terminal window itself and inspect it visually before finishing.

If the change affects core inventory behavior, also run the test executable.

If the app is already running and the linker cannot overwrite the executable, close the running instance first and then rebuild.

## Agent-Owned Verification Sessions

Do not rely on a Inventatory window that the user already has open. Agents may not reliably see minimized, background, or user-focused programs, and using the user's live window can mix verification with the user's own work.

For verification, always launch a fresh Inventatory instance that belongs to the agent:

- Rebuild first in the generic build folder, then launch the already-built executable for visual verification. Do not use `run.ps1` for visual QA because it rebuilds and then detaches the app in a way that makes ownership and window capture harder to prove.
- Launch the app in a real visible desktop terminal window that the user can see and the agent can inspect. Do not use a temporary `USERPROFILE`, hidden process, minimized process, detached process, or background-only launch for visual verification.
- Use the normal user profile/data for visual verification unless the user explicitly asks for a temporary test profile.
- Give the verification terminal a unique title such as `Inventatory-Agent-Verify-<timestamp>` so the screenshot tool can identify the correct window without touching an unrelated terminal.
- For a plain CMD verification window, launch with a command shaped like:

```powershell
$title = "Inventatory-Agent-Verify-" + (Get-Date -Format "yyyyMMdd-HHmmss")
$exe = (Resolve-Path ".\build\Debug\inventatory.exe").Path
Start-Process -FilePath "$env:ComSpec" -WorkingDirectory (Split-Path $exe) -ArgumentList "/k", "title $title && `"$exe`""
```

- If screenshot QA is needed, capture the MAXIMISED FULL SCREEN terminal window whose title contains the unique verification title. Windows Terminal may append the running executable path to the title. Use a window-only capture method such as `PrintWindow` against that exact window handle.
- If `PrintWindow` returns a blank frame, retry with a different window-only capture path that still targets the exact verification window rather than the desktop.
- The verification window must remain visible and inspectable; do not rely on a shell process that cannot be seen on the desktop.
- Do not use full-desktop or screen-pixel screenshots for terminal QA; they can capture whatever is behind the window.
- If the terminal must be made visible for interactive inspection, keep it on the agent-owned window only, not on a user-owned Inventatory session.
- Do not inspect, drive, or capture a user-owned Inventatory window unless the user explicitly asks you to use that exact window.
- If the app throws a startup error dialog, opens the wrong surface, or the captured image is not the Inventatory terminal, close only that failed agent-owned verification window before relaunching. Do not spawn repeated extra windows.
- If a user-owned or stale Inventatory instance locks `build\Debug\inventatory.exe`, close only the Inventatory verification process you started when possible. Ask before closing anything that may belong to the user.
- Record that visual verification used the normal `Documents/Inventatory` data, unless the user explicitly requested otherwise.

## TUI Visual QA

When you change terminal rendering, do not stop at a successful build.

- Launch the app.
- Capture only the MAXIMISED FULL SCREEN terminal window itself, and make sure it is the visible agent-owned terminal window rather than a hidden host or orphaned shell process.
- If you need to move between screens, prefer a fresh agent-owned visible terminal window with direct manual interaction, or restrict automated capture to static/idle screens that do not require input replay.
- Inspect the screenshot for alignment, spacing, clipping, wrapping, color contrast, row striping, and whether animated or scrolling sections look correct.
- Compare the screenshot against the requested layout, not just against the code.
- If the screenshot shows the wrong window, the wrong executable, or an old code path, fix that before concluding the task.
- Record the visual result in your reasoning so later agents know the screen was checked, not assumed.

## Test Environment Notes

- During verification, the local shell runner can occasionally fail before command startup with `windows sandbox: spawn setup refresh`.
- If that happens, rerun the build or test command through the escalated shell path instead of spending time on the failing local runner.
- When debugging SQLite/database behavior, prefer a direct test binary run or a small external probe against `Documents/Inventatory/inventory.db` so failures are easier to isolate.

## When Editing

- Keep the codebase lightweight.
- Preserve the existing keyboard shortcuts unless a change explicitly improves them.
- Prefer incremental changes that keep the terminal responsive.
- If you touch rendering code, verify that the screen does not flicker and that redraws are only triggered when needed.
- Use a short standard file header comment on touched C++ files, and add brief comments only where logic is non-obvious.
