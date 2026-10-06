# Rebuilding the verification environment (new container)

The real project cannot download SQLite here (sqlite.org is blocked by the sandbox proxy), so the
helper scripts build with a stub SQLite source plus the system libsqlite3 and a version shim.

1. Packages: `apt-get update && apt-get install -y build-essential cmake pkg-config libsecret-1-dev
   libcurl4-openssl-dev libssl-dev libglib2.0-dev libsqlite3-dev dbus-x11 gnome-keyring locales`
   then `locale-gen pl_PL.UTF-8 de_DE.UTF-8`.
2. Pick a scratch dir `S` and create: `$S/fakesqlite` (copy tools/fakesqlite/*), `$S/ftxui`
   (`git clone https://github.com/ArthurSonzogni/FTXUI.git $S/ftxui` and `git checkout <GIT_TAG pinned in
   CMakeLists.txt>`), and `$S/shim.so` (`g++ -shared -fPIC -o $S/shim.so tools/shim.cpp`; it fakes
   `sqlite3_libversion_number()` to 3053004 so the pinned-version assert passes against the system SQLite).
3. Copy tools/fulltest.sh and tools/run-stage.sh to `$S` and edit the `S=` line at the top of both to your
   scratch dir. Usage: `$S/fulltest.sh <source-dir> <name> [Debug|Release]` configures once into
   `$S/b-<name>-<cfg>`, builds inventatory + inventatory_tests + inventatory_input_tests, then runs the
   tests inside a private D-Bus session with an unlocked gnome-keyring and
   INVENTATORY_REQUIRE_LOCALE_TESTS=1. It prints `TESTS PASSED (<cfg>)` or the failure. First build is
   15-25 min on 4 cores; incremental builds are fast. The first test run after a rebuild can fail once on a
   keyring step; the script retries once.
4. The Windows build cannot be run locally. `.github/workflows/ci.yml` builds and tests it on every push to
   `main` and every pull request. Use the GitHub MCP tools (actions_list list_workflow_runs /
   list_workflow_jobs, get_job_logs with failed job id) to read the result. The Windows test run stops at the
   first failed assert, so after a fix expect to iterate once or twice.
