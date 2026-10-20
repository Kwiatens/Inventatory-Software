#!/usr/bin/env bash
# Smoke-tests a release binary: libraries resolve, it reports its version, the installer
# installs it, and the interface draws and keeps running. Uses an isolated HOME.
set -euo pipefail

binary=${1:?usage: check-linux-runtime.sh <inventatory-binary>}
[ -f "$binary" ] || { printf 'No such file: %s\n' "$binary" >&2; exit 2; }
binary=$(CDPATH= cd -- "$(dirname -- "$binary")" && pwd)/$(basename -- "$binary")
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
command -v python3 >/dev/null || { printf 'python3 is required for the terminal check\n' >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/inventatory-runtime.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
export HOME="$work/home"
mkdir -p "$HOME"

printf '== shared libraries\n'
missing=$(ldd "$binary" | grep 'not found' || true)
if [ -n "$missing" ]; then
  printf '%s\n' "$missing" >&2
  exit 1
fi

printf '== version\n'
version=$("$binary" --version)
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { printf 'Unexpected version output: %s\n' "$version" >&2; exit 1; }

printf '== release installer\n'
package="$work/package"
payload="$work/payload"
mkdir -p "$package" "$payload"
cp -- "$binary" "$payload/inventatory"
tar -C "$payload" -czf "$package/Inventatory-linux-x64.tar.gz" inventatory
sed 's/\r$//' "$repo_root/installer/Install-Inventatory.sh" > "$package/Install-Inventatory.sh"
chmod 755 "$package/Install-Inventatory.sh"
(cd -- "$package" && sha256sum Inventatory-linux-x64.tar.gz Install-Inventatory.sh > SHA256SUMS-linux.txt)
sh "$package/Install-Inventatory.sh"
installed="$HOME/.local/bin/inventatory"
[ -x "$installed" ] || { printf 'The installer did not place an executable\n' >&2; exit 1; }
[ "$("$installed" --version)" = "$version" ] || { printf 'The installed executable reports another version\n' >&2; exit 1; }

printf '== terminal interface\n'
timeout 60 python3 - "$installed" "$HOME" <<'PY'
import fcntl, os, pty, select, signal, struct, sys, termios, time

binary, home = sys.argv[1], sys.argv[2]
seconds, minimum_bytes = 8, 1000
pid, fd = pty.fork()
if pid == 0:
    os.environ.update(HOME=home, TERM="xterm-256color", LANG="C.UTF-8",
                      XDG_CONFIG_HOME=home + "/.config", XDG_DATA_HOME=home + "/.local/share")
    os.execv(binary, [binary])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 120, 0, 0))
received, exited = 0, None
deadline = time.time() + seconds
while time.time() < deadline:
    ready, _, _ = select.select([fd], [], [], 0.2)
    if ready:
        try:
            received += len(os.read(fd, 65536))
        except OSError:
            break
    done, status = os.waitpid(pid, os.WNOHANG)
    if done:
        exited = status
        break
if exited is None:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
if exited is not None:
    print("The interface exited after drawing %d bytes (wait status %d)" % (received, exited))
    sys.exit(1)
if received < minimum_bytes:
    print("The interface drew only %d bytes" % received)
    sys.exit(1)
print("The interface drew %d bytes and kept running for %d seconds" % (received, seconds))
PY

printf 'Linux runtime check passed\n'
