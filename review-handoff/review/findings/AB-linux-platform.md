# Slice A+B: Linux platform layer

## Coverage
Read in full: src/platform/system/BackgroundControllerLinux.cpp, StartupRegistrationLinux.cpp, ConsoleLinux.cpp,
Environment.cpp/.h, src/main.cpp, installer/Install-Inventatory.sh, src/platform/scanner/BleProvisioningServiceLinux.cpp,
HttpServer.cpp/.h, HttpServerConnection.cpp, HttpServerLifecycle.cpp, HttpServerProtocol.cpp (+Internal headers),
SocketPlatform.h, MdnsService.cpp, src/platform/security/CredentialStoreLinux.cpp.
Skipped: AppIconsLinux.h (data). Windows counterparts only glanced at (MdnsService Win branch, HttpServer _WIN32 branches).
Callers checked: AppShell.cpp (shutdown/restart path), SettingsPageSave.cpp, AppActionSupport.h (credential resolution).
One empirical check: ran `avahi-publish-service --interface=lo ...` locally (see S2 #1).

## Findings

### [S2] Linux mDNS discovery can never start: avahi-publish-service has no --interface option
- Location: src/platform/scanner/MdnsService.cpp:255,267
- Category: linux-portability
- Failure scenario: Every Linux launch with the scanner service enabled calls MdnsService::start(). The child execs
  `avahi-publish-service --interface=<if> Inventatory _inventatory._tcp <port> protocol=1`. avahi-publish only knows
  -h -V -s -a -v -d -H --subtype -R -f. getopt rejects the option and the child exits with status 1 within a few ms;
  the 200 ms waitpid(WNOHANG) sees it and start() returns false, so the Scan R1 is never advertised on Linux.
  Verified locally: `avahi-publish-service --interface=lo Inventatory _inventatory._tcp 4711 protocol=1`
  prints "unknown option '--interface=lo'" and exits 1; the same command without the flag publishes fine.
- Evidence:
    const auto interfaceArgument = "--interface=" + interfaceName;
    execlp("avahi-publish-service", "avahi-publish-service", interfaceArgument.c_str(), "Inventatory", ...
    ...
    if (completed == child) return false;
- Confidence: high (reproduced with the installed avahi-utils)
- Proposed test: a seam-level test that builds the argv (extract a pure `buildPublisherArgv(port, iface)`) and asserts it
  contains only options in avahi-publish's documented set; plus a manual/CI check with a fake `avahi-publish-service`
  script on PATH that rejects unknown options and asserts start() returns true.
- Proposed fix (sketch): drop `--interface` (and keep the private-address gate via `-H`/bound address check), or publish
  through the Avahi D-Bus API (EntryGroup.AddService with interface index) which supports per-interface publishing and
  hostname/address control. Note that without an interface argument the service is announced on every interface, so the
  "private LAN only" invariant must be enforced by the existing privateInterfaceName() gate or the D-Bus API.

### [S2] Orphaned avahi-publish-service child survives a SIGKILL of the app and keeps advertising and holding the HTTP listen socket
- Location: src/platform/scanner/MdnsService.cpp:257-270, src/platform/scanner/HttpServerLifecycle.cpp:491, src/platform/scanner/SocketPlatform.h:34-49
- Category: linux-portability (process lifecycle / fd inheritance)
- Failure scenario: (Once S2 #1 is fixed, otherwise unreachable.) The publisher is forked with fork()+execlp from a process
  whose listening socket (socket() without SOCK_CLOEXEC) is already open, and it has no PR_SET_PDEATHSIG. The takeover
  design deliberately SIGKILLs an unresponsive background service (forceStopBackgroundService). The publisher child is then
  reparented to init, keeps advertising a dead service, and keeps a copy of the listening fd, so the port stays bound
  (SO_REUSEADDR does not allow a second listener). The next instance binds preferredPort+1..19, while a stale record and a
  black-hole listener remain. Same for crashes / OOM kills of an interactive instance; under systemd the cgroup kill hides it
  only for the unit case.
- Evidence:
    NativeSocket socketHandle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);   // no SOCK_CLOEXEC
    accept(listeningSocket, ...)                                              // no accept4(SOCK_CLOEXEC)
    const pid_t child = fork(); ... execlp("avahi-publish-service", ...)      // inherits both
- Confidence: medium (mechanism certain; user-visible effect depends on avahi path being fixed)
- Proposed test: start LocalHttpServer, fork+exec `sleep`, kill -9 the parent test child process, assert the port can be
  rebound (or assert /proc/<sleep>/fd has no socket). Simpler: assert FD_CLOEXEC is set on listen and accepted sockets.
- Proposed fix (sketch): socket(..., SOCK_STREAM | SOCK_CLOEXEC), accept4(..., SOCK_CLOEXEC) (Linux branch of
  SocketPlatform.h), prctl(PR_SET_PDEATHSIG, SIGTERM) in the publisher child (check getppid() afterwards), or close all
  fds >= 3 before exec.

### [S2] Listening/accepted sockets and several helper pipes lack CLOEXEC, so spawned helpers inherit them
- Location: HttpServerLifecycle.cpp:491 (socket), HttpServer.cpp:98 (accept), ConsoleLinux.cpp:49,151 (pipe), StartupRegistrationLinux.cpp:150 (pipe), ConsoleLinux.cpp:95-117 (spawnDetached)
- Category: linux-portability
- Failure scenario: (a) openUrl() -> spawnDetached("xdg-open") forks while the scanner HTTP listener is bound. xdg-open
  execs the browser, which (when it is the first instance) lives for hours and inherits the listen socket and any accepted
  client socket. After the user quits or restarts Inventatory (settings change, update, restartDeviceService) the port stays
  bound by the browser, so the new listener silently moves to port+1 and the R1's stored/cached port is dead.
  (b) The clipboard pipe write end is inherited by any concurrently forked daemon (restartAsBackgroundService execs a
  long-lived service); xclip/xsel -in then never sees EOF, runClipboard blocks forever in waitpid on the UI thread.
- Evidence:
    if (pipe(descriptors) != 0) return false;       // ConsoleLinux.cpp:152, no pipe2(O_CLOEXEC)
    NativeSocket socketHandle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
- Confidence: medium (a: certain mechanism, depends on browser startup; b: needs a concurrent fork, low probability)
- Proposed test: after LocalHttpServer::start, spawn `ls -l /proc/self/fd` through the same helper style and assert no
  socket inode is listed; or fcntl(F_GETFD) & FD_CLOEXEC on listenSocket_ in a test hook.
- Proposed fix (sketch): pipe2(O_CLOEXEC) everywhere; SOCK_CLOEXEC/accept4; dup2 onto stdio in the child clears CLOEXEC
  as needed.

### [S2] Scanner HTTP/BLE-adjacent signal handling: sigaction without SA_RESTART makes the reader drop all pending device connections on any signal
- Location: src/platform/system/BackgroundControllerLinux.cpp:193 (sa_flags = 0), src/platform/scanner/HttpServer.cpp:208-213, 221-225, 375-376
- Category: concurrency / linux-portability
- Failure scenario: SIGTERM/SIGINT/SIGHUP/SIGUSR1 (and FTXUI's SIGWINCH on terminal resize) are process-directed and may be
  delivered to the reader thread. select() returns -1/EINTR and the code treats any negative result as fatal: it closes
  EVERY pending client and clears the list. A Scan R1 request mid-upload is dropped on a window resize or on a second
  launch's SIGUSR1; recv() on EINTR is also mapped to "close". In the worker, sendAll() treats send()==-1/EINTR as failure,
  so a response can be lost AFTER the callback committed and the replay counter advanced; the R1's retry then gets 409 on
  the same counter.
- Evidence:
    action.sa_flags = 0;                                      // no SA_RESTART
    const int selected = select(selectDescriptorCount, &readable, nullptr, nullptr, &timeout);
    if (selected < 0) { for (auto& client : pending) closePendingClient(client); pending.clear(); continue; }
    if (written <= 0) return false;                           // sendAll
- Confidence: medium (signal-delivery thread choice is kernel-dependent, but any thread not blocking the signal can get it)
- Proposed test: unit-test the reader with a helper thread that pthread_kill()s the reader thread with SIGUSR1
  (handler installed without SA_RESTART) during a half-sent request and assert the request still completes.
- Proposed fix (sketch): treat errno==EINTR as "retry" in select/recv/send; set SA_RESTART on the controller handlers;
  block the signals in worker threads (pthread_sigmask before creating threads) so only the signal thread/main sees them.

### [S2] resolveWorkspaceScannerCredential cannot tell "keyring locked/absent" from "no credential"; background unit starts before the keyring/session bus is usable
- Location: src/platform/security/CredentialStoreLinux.cpp:47-57, src/app/common/AppActionSupport.h:50-60, StartupRegistrationLinux.cpp:218-222
- Category: linux-portability / correctness
- Failure scenario: The login unit is `WantedBy=default.target` with no ordering. At login, gnome-keyring/kwallet may be
  locked or not yet unlocked and the unlock prompt cannot show (no DISPLAY/WAYLAND_DISPLAY in the user manager yet).
  secret_password_lookup_sync then fails (or blocks, no GCancellable/timeout) and read() returns nullopt, identical to
  "never stored". With config.setupComplete the app lands on RequiresPairing (safe), but the user is told to re-pair a
  working scanner and gets no keyring diagnostic; on a headless service the call can block indefinitely during
  startup. In SettingsPageSave.cpp:114 oldDigiKeySecret=read() is nullopt for the same reason, so a later rollback runs
  CredentialStore::erase() and deletes the user's real secret.
- Evidence:
    if (error != nullptr) g_error_free(error);
    if (secret == nullptr) return std::nullopt;
- Confidence: medium (behaviour of locked keyring is environment-specific; code evidence is clear)
- Proposed test: a CredentialStore seam returning {Found, NotFound, Unavailable}; test that RequiresPairing carries a
  "credential store unavailable" status and that the DigiKey rollback does not erase when the old read was Unavailable.
- Proposed fix (sketch): return a tri-state/optional<error> from read(); add `After=graphical-session.target` (or a
  `Wants=`/`ExecStartPre` wait on the Secret Service) to the unit; use the async API with a timeout.

### [S3] Executable path read from /proc/self/exe keeps the " (deleted)" suffix after the binary is replaced on disk
- Location: BackgroundControllerLinux.cpp:176-185 (executablePath), StartupRegistrationLinux.cpp:46-55 (currentExecutablePath), ConsoleLinux.cpp:337-347
- Category: linux-portability (sibling of the 'update/relaunch' class)
- Failure scenario: Binary replaced while the app runs (reinstall via Install-Inventatory.sh, package manager, update by
  another window). readlink gives "/home/u/.local/bin/inventatory (deleted)". (1) setBackgroundStartupEnabled(true) with no
  registered unit writes ExecStart="/home/u/.local/bin/inventatory\x20(deleted)" and enables it: the login service can
  never start. (2) createDesktopShortcut rewrites Exec="... (deleted)". (3) restartAsBackgroundService fallback
  (no systemd) execl()s a non-existent path in the grandchild, the intermediate exits 0, the 2 s poll fails and
  AppShell.cpp:187-191 then DISABLES the user's background-service preference and removes the unit. Note the file already
  has the fix for this in executableName() (BackgroundControllerLinux.cpp:132-140) but only for name comparison.
- Evidence:
    const auto count = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (static_cast<size_t>(count) < buffer.size()) return filesystem::path(std::string(buffer.data(), count));
- Confidence: medium
- Proposed test: factor the suffix stripping into a pure helper and unit-test "x (deleted)" -> "x"; test unit contents
  generation never contains "(deleted)".
- Proposed fix (sketch): one shared `currentExecutablePath()` in platform/system that strips the suffix and verifies the
  result with stat(); use it from all three call sites.

### [S3] Controller lock probes briefly take the exclusive lock, so a starting service can spuriously lose single-instance acquisition
- Location: BackgroundControllerLinux.cpp:116, 239, 250 (probe), 97 (acquire)
- Category: concurrency
- Failure scenario: backgroundServiceRunning()/interactiveInstanceRunning()/lockedProcessId() test "is it locked?" by
  flock(LOCK_EX|LOCK_NB) and unlock on success. For those microseconds the probing process IS the lock holder. They run in
  10-25 ms polling loops (takeover, restartAsBackgroundService) exactly while the systemd unit is starting and doing
  its own LOCK_NB acquire. If the acquire hits the window it sees EWOULDBLOCK, treats it as "another instance is
  running", returns 0 from main, and systemd (Restart=on-failure) does not restart a clean exit. The fork fallback in
  restartAsBackgroundService rescues this case, but the probe in a second interactive launch can also make
  acquireSingleInstance(false) fail with "already running but could not be reached".
- Evidence:
    if (flock(descriptor, LOCK_EX | LOCK_NB) == 0) { flock(descriptor, LOCK_UN); close(descriptor); return -1; }
    const bool running = flock(descriptor, LOCK_EX | LOCK_NB) != 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
- Confidence: low-medium (tiny window, self-healing in one path)
- Proposed test: stress test: thread A loops backgroundServiceRunning() while thread B repeatedly acquireSingleInstance/
  stop and asserts acquisition never fails.
- Proposed fix (sketch): probe with fcntl(F_OFD_GETLK) on a LOCK_SH/F_RDLCK request, or use LOCK_SH|LOCK_NB for probes
  (shared probe still conflicts with a would-be exclusive acquire, so prefer OFD getlk), or read the PID file + kill(pid,0)
  as the secondary check.

### [S3] BlueZ GATT characteristic lookup accepts characteristics from other cached devices
- Location: src/platform/scanner/BleProvisioningServiceLinux.cpp:200-215, 170-243
- Category: correctness
- Failure scenario: In the non-service branch every GattCharacteristic1 whose UUID matches the setup status/request UUID is
  taken regardless of device (`(void)parentService;`). GetManagedObjects order is hash order. With two R1 units known to
  BlueZ (second one still bonded/cached, or the old cached copy of the target after RemoveDevice+rediscovery),
  statusPath/requestPath end up being the LAST matching pair, i.e. possibly the other device. The final verification
  correctly rejects it, but then all 12 attempts fail with "Scanner setup service is unavailable" although the target's
  characteristics exist. The first branch (service object with a characteristic interface on the same object) can never
  match: a service object has no GattCharacteristic1.
- Evidence:
    if (characteristicUuid == kStatusCharacteristicUuid) {
      // The parent service is checked again below after its object appears.
      statusPath = objectPath;
      (void)parentService;
- Confidence: medium
- Proposed test: pure-function test over a hand-built GVariant a{oa{sa{sv}}} with two devices, interleaved characteristic
  order, expecting the target's paths.
- Proposed fix (sketch): in the characteristic branch require parentService path to belong to a service whose Device ==
  devicePath (collect service->device map in one pass), delete the dead service-branch code.

### [S3] stopDiscovery()/provision() block the calling thread for seconds; discovery cannot be interrupted
- Location: BleProvisioningServiceLinux.cpp:679-685, 743-774, 886-987
- Category: concurrency / ui
- Failure scenario: stopDiscovery() joins a worker that is inside managedObjects(...,3000) or sleeping 400 ms or in
  g_bus_get_sync/StartDiscovery/SetDiscoveryFilter (3 s each, per adapter), so closing the Bluetooth wizard or
  starting provisioning freezes the UI thread up to several seconds (more with several adapters or a hung bluetoothd).
  provision() has timeouts up to 30 s (Pair) + 15 s + 12*5.5 s + 15 s + 25 s with no cancellation; check that the caller runs it
  on a worker (not verified here).
- Evidence:
    if (worker_.joinable()) worker_.join();
    auto* objects = managedObjects(connection, 3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
- Confidence: medium
- Proposed test: with a fake blocking bus (or a short unit test of the loop with an injected call function) assert
  stopDiscovery returns within 500 ms.
- Proposed fix (sketch): use a GCancellable shared with the sync calls and a condition_variable wait instead of sleep_for;
  cancel on stop.

### [S3] Server binds to the first private address found at start and never follows interface changes; docker/libvirt bridges qualify
- Location: HttpServerLifecycle.cpp:395-396, ConsoleLinux.cpp:462-483, MdnsService.cpp:44-62
- Category: linux-portability
- Failure scenario: privateLocalAddresses().front() is whatever getifaddrs yields first among 10/8, 172.16/12, 192.168/16
  and 169.254/16. On Linux hosts with docker0 (172.17.0.1), virbr0 (192.168.122.1), tailscale-less VPN tun (10.x) or a
  link-local address the list order is interface-index order, so a bridge created before Wi-Fi/Ethernet came up (or a
  USB NIC later) can win. The server then listens on a bridge address the R1 cannot reach; there is no fallback and no
  rebind when DHCP renews. Windows has the same single-address design, but docker/libvirt bridges are far more common
  on Linux.
- Evidence:
    const string bindAddress = availableAddresses.empty() ? "127.0.0.1" : availableAddresses.front();
- Confidence: low-medium
- Proposed test: factor address selection into a pure function over a list of (name, address, flags) and test that
  docker0/virbr0/br-*/veth* are de-prioritised behind the default-route interface.
- Proposed fix (sketch): prefer the interface of the default route (/proc/net/route), skip docker/virbr/br-/veth/tailscale
  names, or expose the chosen interface in settings.

### [S3] accept() failure loop can spin at 100% CPU (EMFILE/ENFILE/ENOBUFS)
- Location: src/platform/scanner/HttpServer.cpp:98-102
- Category: correctness
- Failure scenario: If accept() fails persistently (fd exhaustion, kernel memory), `if (running_.load()) continue;` retries
  immediately with no backoff and no error recorded, pegging a core in the background service until fds are freed.
- Evidence:
    if (client == kInvalidSocket) { if (running_.load()) continue; break; }
- Confidence: medium
- Proposed test: set RLIMIT_NOFILE low in a test, open connections, assert CPU time stays bounded (or assert a backoff
  helper is invoked).
- Proposed fix (sketch): on errno other than EINTR/ECONNABORTED sleep 50-100 ms (or poll with timeout) before retry.

### [S3] systemctl/zenity helpers: no timeout, stderr inherited into the TUI, and the unit helper can block the UI for 15 s+
- Location: StartupRegistrationLinux.cpp:148-196, ConsoleLinux.cpp:48-93
- Category: linux-portability / ui
- Failure scenario: runSystemctl() blocks reading the pipe until EOF with no deadline. `disable --now` waits for
  TimeoutStopSec=15; a wedged user manager hangs the settings Save on the UI thread indefinitely. runCaptured() (zenity)
  redirects only stdout: GTK warnings ("cannot open display", dconf, Gtk-Message) go to the inherited stderr, i.e. onto
  the FTXUI alternate screen, corrupting the display until the next full redraw. runCaptured also waitpid()s a child that
  is still alive (output > 32 KiB branch).
- Evidence:
    if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(127);   // stderr not redirected
    for (;;) { const auto count = read(outputPipe[0], buffer, sizeof(buffer)); if (count == 0) break; ...
- Confidence: medium
- Proposed test: fake `systemctl`/`zenity` scripts on PATH (sleep 30; echo warning >&2) and assert a bounded return and
  no bytes on the parent's stderr.
- Proposed fix (sketch): dup2 stderr to /dev/null (zenity) or to the pipe (already done for systemctl); poll() with a
  deadline then SIGKILL the child.

### [S3] Lock directory differs when XDG_RUNTIME_DIR is unset, defeating single-instance/takeover between launch contexts
- Location: BackgroundControllerLinux.cpp:69-84
- Category: linux-portability
- Failure scenario: The systemd user unit always has XDG_RUNTIME_DIR; a launch from cron, `su -`, ssh without a PAM
  session, or some terminal wrappers does not and uses /tmp/inventatory-<uid>. The two processes then take locks in
  different directories, see neither the service nor each other, and run two instances on the same workspace/SQLite DB
  (the exact condition takeover exists to prevent).
- Evidence: `const auto fallback = filesystem::path("/tmp") / ("inventatory-" + std::to_string(geteuid()));`
- Confidence: low
- Proposed test: run controller with XDG_RUNTIME_DIR set in one and unset in another process, assert second acquire fails.
- Proposed fix (sketch): derive /run/user/<uid> when it exists and is owned by the user even if the env var is missing.

### [S3] Unit file is overwritten before systemctl enable; failed enable on an existing unit is not rolled back
- Location: StartupRegistrationLinux.cpp:393-416
- Category: persistence
- Failure scenario: When the unit existed and its contents changed (e.g. registered executable removed, launched from a
  new location), writeFileAtomically replaces it, then daemon-reload/enable fails (user manager down). `existed` is true so no
  rollback: the old, working unit text is gone and the returned error leaves a half-applied state. Idempotency itself
  (no rewrite when unchanged) is correct.
- Confidence: low
- Proposed test: fake systemctl that fails `enable`; assert the previous unit contents are restored.
- Proposed fix (sketch): keep the old contents in memory and restore on failure.

### [S4-ish/S3] Desktop Exec quoting does not escape $ and ` and double-escaping of backslash
- Location: StartupRegistrationLinux.cpp:79-92
- Category: correctness
- Failure scenario: Per the Desktop Entry spec, inside quotes `"`, `` ` ``, `$` and `\` must be backslash-escaped and then
  backslash itself doubled for the key-file string layer. A path containing `$` or a backtick is left unescaped; a
  backslash is emitted as `\\` (one layer) so it decodes to a single `\` that the Exec parser then eats. Rare paths, but
  the function is advertised to handle special paths (and the unit writer handles `$`).
- Confidence: low
- Proposed test: round-trip test with paths containing $ ` \ " space and % through a spec-conformant Exec parser (GLib
  g_shell_parse_argv after unescaping).
- Proposed fix (sketch): escape `$` and backtick, and double every backslash.

## Test gaps
- MdnsService (Linux): argv/option set, start() failure vs success, stop() kills and reaps child, orphan behaviour -> fake `avahi-publish-service` script on PATH.
- BlueZ code (BleProvisioningServiceLinux.cpp): findGattCharacteristics / findDevicePath / parseBluetoothAddress / validators are pure functions over GVariant; no test calls them. Add GVariant fixtures including two devices and interleaved order.
- LocalHttpServer sockets: CLOEXEC flags, EINTR in select/recv/send, accept error backoff, header/body limit boundaries (8 KiB header / 64 KiB body exactly at limit and +1), slow-loris deadline (2 s), Content-Length with whitespace/+/hex, duplicate Content-Length, transfer-encoding -> only replay/auth paths are covered today.
- BackgroundController Linux: signal-handler EINTR behaviour, probe-vs-acquire race, stale PID in lock file, pidfd path vs kill fallback, runtime dir validation (wrong owner/mode/symlink), XDG_RUNTIME_DIR unset.
- StartupRegistrationLinux: systemdQuoted/systemdUnquote round trip (spaces, %, $, quotes, non-ASCII), "(deleted)" path, enable-failure rollback, disable when user manager unavailable.
- CredentialStoreLinux: behaviour when the Secret Service is absent/locked (read vs not-found distinction).
- Install-Inventatory.sh: no automated test (tamper checksum, path traversal member, symlink member, parent PID wait, rollback on marker failure); a CI shell test with a crafted tarball would cover the validate_archive logic.
- ConsoleLinux: ensureTerminalAttached candidate arg builders for ptyxis/kitty/foot/wezterm/xterm (only konsole covered), executableAvailable with empty PATH entries, openUrl validation matrix.

## Quality (S4)
### [S4] findGattCharacteristics is convoluted and partly dead
- Location: BleProvisioningServiceLinux.cpp:170-243
- Dead service-object branch (no GattCharacteristic1 on a service object), unused `parentService` variable, and the
  double-iteration verification; rewrite as: build map service->device, then pick characteristics whose Service maps to devicePath.

### [S4] Duplicated helpers
- /proc/self/exe readlink loop in BackgroundControllerLinux.cpp:176, StartupRegistrationLinux.cpp:46, ConsoleLinux.cpp:337; PATH-search in ConsoleLinux.cpp:32 and MdnsService.cpp:64; backgroundServiceRunning/interactiveInstanceRunning are copy-paste; fork/pipe/exec helpers repeated in Console and StartupRegistration (runSystemctl, runCaptured, runClipboard). A single `platform/system/Process.{h,cpp}` (spawn with CLOEXEC pipe, timeout, stderr policy) would remove ~150 lines and fix the CLOEXEC/timeout findings once.

### [S4] Dead code / misleading names
- HttpServer.cpp:35 `kWorkerCount` duplicate of HttpServerLifecycle.cpp:26 (unused in HttpServer.cpp); `networkStarted_` unused on Linux.
- HttpServerProtocol.cpp:19 `jsonEscape` in http_server_detail is unreferenced (core scanner has its own).
- HttpServerConnection.cpp:213-215: `headerEnd > kMaxHttpHeaderBytes` checked after the whole request is already buffered; harmless but the reader already enforces it.
- ConsoleLinux.cpp:485-486 controlModifierPressed()/pollKeys() are stubs: on Linux, Ctrl+Backspace distinction in AppShell.cpp:55 can never trigger (Event::CtrlH is mapped separately, so behaviour is acceptable but undocumented).
- BleProvisioningServiceLinux.cpp:263-264 second lookup for "ay" is identical to G_VARIANT_TYPE_BYTESTRING? (bytestring is "ay"), so the fallback is dead.
- Secrets hygiene: GVariant holding Wi-Fi password/token (line 969) is freed unzeroed even though `payload` is wiped at line 973.
- setBackgroundStartupEnabled(false) path returns error when the user manager is unavailable and leaves the unit file, so a user without systemd --user can never clear the setting in-app.
