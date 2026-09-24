#!/bin/sh
set -eu
umask 077

archive_name='Inventatory-linux-x64.tar.gz'
checksums_name='SHA256SUMS-linux.txt'
installer_name='Install-Inventatory.sh'
update_mode=0
marker_path=''
release_version=''
target=''
stage=''

write_marker() {
  [ -n "$marker_path" ] || return 0
  marker_dir=$(dirname -- "$marker_path")
  mkdir -p -- "$marker_dir" || return 1
  marker_tmp="$marker_path.tmp-$$"
  safe_error=$(printf '%s' "${2-}" | tr '\r\n' '  ' | cut -c 1-512)
  {
    printf 'schema_version=1\n'
    printf 'state=%s\n' "$1"
    printf 'version=%s\n' "$release_version"
    printf 'error=%s\n' "$safe_error"
  } > "$marker_tmp" || return 1
  chmod 600 -- "$marker_tmp" || return 1
  mv -f -- "$marker_tmp" "$marker_path"
}

cleanup() {
  if [ -n "$stage" ] && [ -d "$stage" ]; then rm -rf -- "$stage"; fi
}

fail() {
  message=$1
  write_marker failed "$message" >/dev/null 2>&1 || true
  printf 'Inventatory installation failed: %s\n' "$message" >&2
  exit 1
}

verify_asset() {
  file_path=$1
  asset_name=$2
  expected=$(awk -v name="$asset_name" '
    ($2 == name || $2 == "*" name) && length($1) == 64 && $1 ~ /^[0-9A-Fa-f]+$/ { count++; digest=tolower($1) }
    END { if (count != 1) exit 1; print digest }
  ' "$checksums_path") || fail "Checksum manifest has no unique entry for $asset_name"
  actual=$(sha256sum -- "$file_path" | awk '{ print tolower($1) }') || fail "Unable to hash $asset_name"
  [ "$actual" = "$expected" ] || fail "Checksum verification failed for $asset_name"
}

validate_archive() {
  listing="$stage/archive-list.txt"
  details="$stage/archive-details.txt"
  tar -tzf "$archive_path" > "$listing" 2>/dev/null || fail 'The Linux update archive is invalid'
  tar -tvzf "$archive_path" > "$details" 2>/dev/null || fail 'The Linux update archive could not be inspected'
  while IFS= read -r member; do
    case "$member" in
      inventatory|LICENSE|LICENSE.md|README.md|THIRD_PARTY_NOTICES.md|docs/|docs/linux-support.md) ;;
      *) fail 'The Linux update archive contains an unexpected path' ;;
    esac
  done < "$listing"
  while IFS= read -r entry; do
    kind=$(printf '%s' "$entry" | cut -c 1)
    case "$kind" in
      -) ;;
      d)
        member=${entry##* }
        [ "$member" = 'docs/' ] || fail 'The Linux update archive contains an unexpected directory'
        ;;
      *) fail 'The Linux update archive contains a non-regular file' ;;
    esac
  done < "$details"
  tar -xzf "$archive_path" -C "$stage" 2>/dev/null || fail 'The Linux update archive could not be extracted'
  [ -f "$stage/inventatory" ] && [ ! -L "$stage/inventatory" ] || fail 'The Linux archive does not contain the Inventatory executable'
  [ -x "$stage/inventatory" ] || chmod 755 -- "$stage/inventatory" || fail 'The Linux executable could not be prepared'
}

if [ "${1-}" = '--update' ]; then
  [ "$#" -eq 7 ] || fail 'The update installer arguments are incomplete'
  update_mode=1
  archive_path=$2
  checksums_path=$3
  marker_path=$4
  notes_path=$5
  release_version=$6
  parent_pid=$7
  case "$release_version" in
    ''|*[!A-Za-z0-9.+-]*) fail 'The update release identity is invalid' ;;
  esac
  case "$parent_pid" in
    ''|*[!0-9]*) fail 'The Inventatory process identity is invalid' ;;
  esac
  target=$(readlink -f -- "/proc/$parent_pid/exe" 2>/dev/null) || fail 'Could not locate the running Inventatory executable'
  [ "$(basename -- "$target")" = 'inventatory' ] || fail 'The running executable path is not an Inventatory Linux installation'
  [ -f "$target" ] && [ ! -L "$target" ] || fail 'The running executable could not be safely replaced'
  trap cleanup EXIT HUP INT TERM
  stage=$(mktemp -d "$(dirname -- "$target")/.inventatory-update.XXXXXX") || fail 'The installation folder is not writable'
  verify_asset "$archive_path" "$archive_name"
  verify_asset "$0" "$installer_name"
  validate_archive
  tries=0
  while [ -e "/proc/$parent_pid/exe" ]; do
    current=$(readlink -f -- "/proc/$parent_pid/exe" 2>/dev/null || true)
    [ "$current" = "$target" ] || break
    [ "$tries" -lt 300 ] || fail 'Inventatory did not close in time; the existing executable was left unchanged'
    tries=$((tries + 1))
    sleep 0.1
  done
else
  script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
  archive_path="$script_dir/$archive_name"
  checksums_path="$script_dir/$checksums_name"
  [ -f "$archive_path" ] && [ -f "$checksums_path" ] || fail 'Place the Linux archive and checksum manifest beside this installer'
  target_dir="$HOME/.local/bin"
  mkdir -p -- "$target_dir" || fail 'Unable to create ~/.local/bin'
  target="$target_dir/inventatory"
  stage=$(mktemp -d "$target_dir/.inventatory-install.XXXXXX") || fail 'Unable to create a staging directory'
  trap cleanup EXIT HUP INT TERM
  verify_asset "$archive_path" "$archive_name"
  verify_asset "$0" "$installer_name"
  validate_archive
fi

target_dir=$(dirname -- "$target")
[ -w "$target_dir" ] || fail 'The executable folder is not writable; install Inventatory in a user-writable location'
chmod 755 -- "$stage/inventatory" || fail 'Unable to set executable permissions'
if [ "$update_mode" -eq 1 ]; then
  cp -p -- "$target" "$stage/previous-inventatory" || fail 'Unable to preserve the current executable before updating'
fi
mv -f -- "$stage/inventatory" "$target" || fail 'Unable to activate the new executable; the previous executable was left unchanged'
if ! write_marker complete ''; then
  if [ "$update_mode" -eq 1 ]; then
    mv -f -- "$stage/previous-inventatory" "$target" || fail 'Update status could not be saved and rollback failed'
  fi
  fail 'The update completion status could not be saved'
fi

if [ "$update_mode" -eq 1 ]; then
  printf 'Inventatory %s was updated at %s\n' "$release_version" "$target"
else
  printf 'Inventatory was installed to %s\n' "$target"
  printf 'Run it with: %s\n' "$target"
fi
