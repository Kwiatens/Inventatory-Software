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
