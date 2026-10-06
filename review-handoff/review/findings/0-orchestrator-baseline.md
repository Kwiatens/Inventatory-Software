# Phase 0: Baseline machine checks (orchestrator)

- Clang ASan+UBSan Debug build, full ctest: **clean**. No memory or UB errors on the paths the tests exercise.
- Clang strict-warning Release build, ctest: passes.
- GCC sanitizer runtime (libasan/libubsan) is not installed locally, so Clang was used instead.

## Findings

### [S2] inventatory_input test is a no-op in Release builds — CONFIRMED
- Location: tests/terminal_input_parser_tests.cpp:1, CMakeLists.txt:334-344
- Category: test-gap
- Failure scenario: Release CI (release.yml runs Release ctest) defines NDEBUG, so every standard `assert` in this file compiles away. The test always passes (0.00 s) even if the pinned FTXUI Escape-parser fix regresses. inventatory_tests.cpp avoids this with its own macro (`#undef assert` at line 78); this file does not.
- Fix: use the same always-on check macro (or `#undef NDEBUG` before `<cassert>`), ideally in a shared tests/TestAssert.h.

### [S4] Dead code found by -Wunused
- core/history/InventoryHistory.cpp:16,22,29 — historyFilePath, serializeHistoryPoint, deserializeHistoryPoint are unused.
- core/query/InventoryQueryMatching.cpp:25 — splitTokensRespectingQuotes is unused.
- ui/ActionRegistry.cpp:26 — `none()` is unused.
- ui/pages/history/HistoryPagePrivate.cpp:39 — sameDate is unused.
- platform/scanner/HttpServer.cpp:35 — kWorkerCount is unused.
- ui/pages/scanner/InventatoryScanSetupPage.cpp:21 — kDebugWindowLines is unused.

### [S4] Misc compiler diagnostics
- app/scanner/AppScanEnrichment.cpp:151,263 — lambdas capture structured bindings, which is a C++20 extension in a project that declares C++17 (portability).
- ui/pages/bom/BomProjectPageRender.cpp:484 — pessimizing std::move of a temporary.
- app.cpp:44 — member init order differs from declaration order (-Wreorder-ctor).
- AppSettings.cpp:327 and UpdateService.cpp:132 — mixed `&&`/`||` without parentheses. The precedence is correct as written; add parentheses for readability.
- AppInput.cpp:196, AppShellRender.cpp:271, ActionRegistry.cpp:60 — switches don't handle all enum values (Onboarding, ScanSetup, DigiKeySetup…). Verify the default fallthrough is intended.
- 388 × unqualified `move(...)` (from `using namespace std;`), which Clang flags. Style only; leave as is unless doing a broader cleanup.
