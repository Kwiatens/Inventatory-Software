#!/usr/bin/env bash
set -euo pipefail

installer_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root=${INVENTATORY_REPO_ROOT:-$(CDPATH= cd -- "$installer_dir/.." && pwd)}
test_root="$(mktemp -d "${TMPDIR:-/tmp}/inventatory-linux-installer-smoke.XXXXXX")"
trap 'rm -rf -- "$test_root"' EXIT

package_dir="$test_root/package"
payload_dir="$test_root/payload"
install_home="$test_root/home"
mkdir -p -- "$package_dir" "$payload_dir" "$install_home"

cat > "$payload_dir/inventatory" <<'FIXTURE'
#!/bin/sh
printf 'Inventatory installer fixture\n'
FIXTURE
chmod 755 -- "$payload_dir/inventatory"
tar -C "$payload_dir" -czf "$package_dir/Inventatory-linux-x64.tar.gz" inventatory
sed 's/\r$//' "$repo_root/installer/Install-Inventatory.sh" > "$package_dir/Install-Inventatory.sh"
chmod 755 -- "$package_dir/Install-Inventatory.sh"
(
  cd -- "$package_dir"
  sha256sum Inventatory-linux-x64.tar.gz Install-Inventatory.sh > SHA256SUMS-linux.txt
  HOME="$install_home" sh "$package_dir/Install-Inventatory.sh"
)

installed="$install_home/.local/bin/inventatory"
test -x "$installed"
version=$("$installed" --version)
test "$version" = 'Inventatory installer fixture'
printf 'Linux installer smoke test passed: %s\n' "$version"

# Refuse an unstartable executable before activation.
broken_payload="$test_root/broken-payload"
broken_package="$test_root/broken-package"
broken_home="$test_root/broken-home"
mkdir -p -- "$broken_payload" "$broken_package" "$broken_home"
cat > "$broken_payload/inventatory" <<'FIXTURE'
#!/bin/sh
printf 'fixture loader error: version GLIBC_9.9 not found\n' >&2
exit 1
FIXTURE
chmod 755 -- "$broken_payload/inventatory"
tar -C "$broken_payload" -czf "$broken_package/Inventatory-linux-x64.tar.gz" inventatory
sed 's/\r$//' "$repo_root/installer/Install-Inventatory.sh" > "$broken_package/Install-Inventatory.sh"
chmod 755 -- "$broken_package/Install-Inventatory.sh"
(
  cd -- "$broken_package"
  sha256sum Inventatory-linux-x64.tar.gz Install-Inventatory.sh > SHA256SUMS-linux.txt
)
if HOME="$broken_home" sh "$broken_package/Install-Inventatory.sh" > "$test_root/broken-install.log" 2>&1; then
  printf 'An executable that cannot start was installed\n' >&2
  exit 1
fi
grep -q 'does not start on this system: fixture loader error' "$test_root/broken-install.log"
test ! -e "$broken_home/.local/bin/inventatory"
printf 'Linux installer refusal of an unstartable executable test passed\n'

# In-app update: the installer replaces the running executable, then removes the download folder.
live_dir="$test_root/live"
mkdir -p -- "$live_dir"
# Copy a real binary: on RHEL-family systems sleep is a wrapper and would run as coreutils.
sleep_binary=$(command -v sleep)
sleep_program=
if [ "$(head -c 2 -- "$sleep_binary")" = '#!' ]; then
  sleep_binary=$(command -v coreutils)
  sleep_program='--coreutils-prog=sleep'
fi

run_update_case() {
  case_name=$1
  tamper=$2
  replace_live=${3:-keep}
  download_dir="$test_root/Inventatory-update-$case_name"
  marker="$test_root/$case_name-marker"
  mkdir -p -- "$download_dir"
  chmod 700 -- "$download_dir"
  cp -- "$package_dir/Inventatory-linux-x64.tar.gz" "$download_dir/"
  cp -- "$package_dir/Install-Inventatory.sh" "$download_dir/"
  (cd -- "$download_dir" && sha256sum Inventatory-linux-x64.tar.gz Install-Inventatory.sh > SHA256SUMS-linux.txt)
  if [ "$tamper" = tamper ]; then printf 'tampered' >> "$download_dir/Inventatory-linux-x64.tar.gz"; fi
  cp -- "$sleep_binary" "$live_dir/inventatory"
  "$live_dir/inventatory" ${sleep_program:+"$sleep_program"} 30 &
  live_pid=$!
  if [ "$replace_live" = replace ]; then
    # Replacing the file on disk makes /proc/<pid>/exe end in " (deleted)".
    cp -- "$sleep_binary" "$live_dir/inventatory.new"
    mv -f -- "$live_dir/inventatory.new" "$live_dir/inventatory"
    case "$(readlink "/proc/$live_pid/exe")" in
      *' (deleted)') ;;
      *) printf 'Fixture process did not report a deleted executable\n' >&2; exit 1 ;;
    esac
  fi
  sh "$download_dir/Install-Inventatory.sh" --update "$download_dir/Inventatory-linux-x64.tar.gz" \
    "$download_dir/SHA256SUMS-linux.txt" "$marker" "$test_root/notes" 9.9.9 "$live_pid" &
  installer_pid=$!
  sleep 1
  kill "$live_pid" 2>/dev/null || true
  wait "$live_pid" 2>/dev/null || true
  wait "$installer_pid" || true
  if [ -e "$download_dir" ]; then
    printf 'Update download folder was left behind (%s)\n' "$case_name" >&2
    exit 1
  fi
}

run_update_case good ok
grep -q '^state=complete$' "$test_root/good-marker"
test "$("$live_dir/inventatory" --version)" = 'Inventatory installer fixture'

run_update_case bad tamper
grep -q '^state=failed$' "$test_root/bad-marker"
printf 'Linux update hand-off test passed\n'

# An executable replaced while running is still updated in place.
run_update_case replaced ok replace
grep -q '^state=complete$' "$test_root/replaced-marker"
test "$("$live_dir/inventatory" --version)" = 'Inventatory installer fixture'
printf 'Linux update of a replaced executable test passed\n'
