#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

ROOT=$(nr_root)
CHECK_ONLY=0
WITH_RELEASE_DEPS=0

usage() {
  cat <<'EOF'
Usage: scripts/bootstrap.sh [--check] [--with-release-deps]

  --check              Validate pins and required tools without fetching.
  --with-release-deps  Download and verify the pinned ESP32-C6 release binary
                       into ignored build/deps storage.
EOF
}

while (($#)); do
  case "$1" in
    --check) CHECK_ONLY=1 ;;
    --with-release-deps) WITH_RELEASE_DEPS=1 ;;
    -h|--help) usage; exit 0 ;;
    *) nr_die "unknown option: $1" ;;
  esac
  shift
done

nr_require_tool git
nr_require_tool python3
nr_require_tool tar
[[ -f $ROOT/sources.lock.json ]] || nr_die "missing sources.lock.json"
[[ -f $ROOT/profiles/manifest.json ]] || nr_die "missing profiles/manifest.json"

nr_python - "$ROOT/sources.lock.json" <<'PY'
import json
import re
import sys

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    lock = json.load(stream)
if lock.get("schema_version") != 1:
    raise SystemExit("sources.lock.json: schema_version must be 1")
for source in ("nuttx", "apps"):
    item = lock.get("upstreams", {}).get(source, {})
    if not re.fullmatch(r"[0-9a-f]{40}", item.get("commit", "")):
        raise SystemExit(f"sources.lock.json: invalid full commit for {source}")
    if not item.get("url", "").startswith("https://github.com/apache/"):
        raise SystemExit(f"sources.lock.json: {source} must use the Apache upstream URL")
build = lock.get("build", {})
digest = build.get("container_digest", "")
if not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
    raise SystemExit("sources.lock.json: build container must be pinned by sha256 digest")
dependencies = lock.get("dependencies", {})
for name in ("berry", "esp_hosted_mcu"):
    sha = dependencies.get(name, {}).get("sha256", "")
    if not re.fullmatch(r"[0-9a-f]{64}", sha):
        raise SystemExit(f"sources.lock.json: invalid sha256 for {name}")
pico = dependencies.get("pico_sdk", {})
for key in ("commit", "cyw43_driver_commit"):
    if not re.fullmatch(r"[0-9a-f]{40}", pico.get(key, "")):
        raise SystemExit(f"sources.lock.json: invalid pico_sdk {key}")
if not re.fullmatch(r"[0-9a-f]{64}", pico.get("cyw43439_firmware_header_sha256", "")):
    raise SystemExit("sources.lock.json: invalid CYW43439 firmware header sha256")
if pico.get("cyw43_driver_commit", "") not in pico.get("firmware_header_url", ""):
    raise SystemExit("sources.lock.json: CYW43439 URL must contain the pinned driver commit")
PY

for source in nuttx apps; do
  commit=$(nr_lock_commit "$ROOT" "$source")
  gitlink=$(nr_gitlink_commit "$ROOT" "$source")
  [[ $gitlink == "$commit" ]] || \
    nr_die "${source} gitlink ${gitlink:-missing} does not match lock ${commit}"

  if ((CHECK_ONLY)); then
    git -C "$ROOT/$source" cat-file -e "${commit}^{commit}" 2>/dev/null || \
      nr_die "${source} object ${commit} is unavailable; run bootstrap without --check"
  else
    git -C "$ROOT" submodule sync --recursive -- "$source"
    nr_ensure_commit "$ROOT" "$source" "$commit" "$(nr_lock_url "$ROOT" "$source")"
  fi
done

download_release_dependency() {
  local relative_path
  local destination
  local url
  local expected

  relative_path=$(nr_json_value "$ROOT/sources.lock.json" dependencies.esp_hosted_mcu.path)
  url=$(nr_json_value "$ROOT/sources.lock.json" dependencies.esp_hosted_mcu.url)
  expected=$(nr_json_value "$ROOT/sources.lock.json" dependencies.esp_hosted_mcu.sha256)
  [[ $relative_path != /* && $relative_path != *../* && $relative_path != ../* ]] || \
    nr_die "unsafe esp_hosted_mcu.path in sources.lock.json"
  destination=$ROOT/$relative_path
  mkdir -p "$(dirname -- "$destination")"

  if [[ -f $destination ]] && [[ $(nr_sha256 "$destination") == "$expected" ]]; then
    nr_note "release dependency already verified: ${relative_path}"
    return
  fi
  ((CHECK_ONLY == 0)) || nr_die "release dependency is missing or invalid: ${relative_path}"

  nr_download_verified "$url" "$expected" "$destination"
  nr_note "downloaded verified release dependency: ${relative_path}"
}

download_berry_dependency() {
  local commit
  local url
  local expected
  local destination

  commit=$(nr_json_value "$ROOT/sources.lock.json" dependencies.berry.version dependencies.berry.commit)
  url=$(nr_json_value "$ROOT/sources.lock.json" dependencies.berry.url)
  expected=$(nr_json_value "$ROOT/sources.lock.json" dependencies.berry.sha256)
  destination=$ROOT/build/deps/berry-${commit}.zip
  if ((CHECK_ONLY)); then
    if [[ -f $destination ]]; then
      [[ $(nr_sha256 "$destination") == "$expected" ]] || \
        nr_die "cached Berry archive failed checksum validation"
    fi
    return
  fi
  nr_download_verified "$url" "$expected" "$destination"
  nr_note "verified pinned Berry source archive"
}

download_pico_firmware_dependency() {
  local version
  local url
  local expected
  local destination

  version=$(nr_json_value "$ROOT/sources.lock.json" dependencies.pico_sdk.version)
  url=$(nr_json_value "$ROOT/sources.lock.json" dependencies.pico_sdk.firmware_header_url)
  expected=$(nr_json_value "$ROOT/sources.lock.json" dependencies.pico_sdk.cyw43439_firmware_header_sha256)
  destination=$ROOT/build/deps/pico-sdk-${version}/lib/cyw43-driver/firmware/w43439A0_7_95_49_00_combined.h
  if ((CHECK_ONLY)); then
    if [[ -f $destination ]]; then
      [[ $(nr_sha256 "$destination") == "$expected" ]] || \
        nr_die "cached CYW43439 firmware header failed checksum validation"
    fi
    return
  fi
  nr_download_verified "$url" "$expected" "$destination"
  nr_note "verified pinned Pico SDK CYW43439 firmware header"
}

if ((WITH_RELEASE_DEPS)); then
  download_release_dependency
fi
download_berry_dependency
download_pico_firmware_dependency

mkdir -p "$ROOT/build/work" "$ROOT/dist" "$ROOT/logs"
"$SCRIPT_DIR/check-profiles.py"
nr_note "bootstrap validation complete"
