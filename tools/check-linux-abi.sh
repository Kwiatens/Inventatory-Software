#!/usr/bin/env bash
# Fails when a Linux release binary needs a symbol or library version newer than the oldest supported
# distribution provides. The release is built in an AlmaLinux 9 container (glibc 2.34, GCC 11), so the
# defaults are that container's floor: glibc 2.34, libstdc++ GLIBCXX_3.4.29, C++ ABI 1.3.13, OpenSSL 3.0.0.
set -euo pipefail

binary=${1:?usage: check-linux-abi.sh <binary> [max-glibc] [max-glibcxx] [max-cxxabi]}
max_glibc=${2:-2.34}
max_glibcxx=${3:-3.4.29}
max_cxxabi=${4:-1.3.13}
max_openssl=3.0.0

[ -f "$binary" ] || { printf 'No such file: %s\n' "$binary" >&2; exit 2; }
command -v readelf >/dev/null || { printf 'readelf is required (install binutils)\n' >&2; exit 2; }

# True when the version is a plain dotted number no newer than the limit.
within_limit() {
  case "$1" in
    ''|*[!0-9.]*) return 1 ;;
  esac
  [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -n 1)" = "$2" ]
}

# readelf lists the version needs as a "File:" header followed by its "Name:" entries.
requirements=$(readelf -W -V "$binary" | awk '
  /Version needs section/ { needs = 1; next }
  /Version definition section|Version symbols section/ { needs = 0 }
  needs && /File:/ { sub(/.*File: /, ""); file = $1 }
  needs && /Name:/ { sub(/.*Name: /, ""); print file, $1 }
')

status=0
reject() {
  printf 'Not portable: %s needs %s, which is newer than the supported floor\n' "$1" "$2" >&2
  status=1
}
reject_library() {
  printf 'Not portable: %s needs %s, a version tag that other distributions do not provide\n' "$1" "$2" >&2
  status=1
}

while read -r file name; do
  [ -n "${name:-}" ] || continue
  case "$file:$name" in
    libc.so.6:GLIBC_*|libm.so.6:GLIBC_*|ld-linux-x86-64.so.2:GLIBC_*)
      within_limit "${name#GLIBC_}" "$max_glibc" || reject "$file" "$name" ;;
    libstdc++.so.6:GLIBCXX_*)
      within_limit "${name#GLIBCXX_}" "$max_glibcxx" || reject "$file" "$name" ;;
    libstdc++.so.6:CXXABI_*)
      within_limit "${name#CXXABI_}" "$max_cxxabi" || reject "$file" "$name" ;;
    libgcc_s.so.1:GCC_*) ;;
    libcrypto.so.3:OPENSSL_*)
      within_limit "${name#OPENSSL_}" "$max_openssl" || reject "$file" "$name" ;;
    *)
      # Any other versioned library (for example a distribution-specific libcurl) may be missing that tag on another distribution.
      reject_library "$file" "$name" ;;
  esac
done <<< "$requirements"

if [ "$status" -eq 0 ]; then
  printf 'Linux ABI check passed: glibc <= %s, libstdc++ <= %s, C++ ABI <= %s, OpenSSL <= %s\n' \
    "$max_glibc" "$max_glibcxx" "$max_cxxabi" "$max_openssl"
fi
exit "$status"
