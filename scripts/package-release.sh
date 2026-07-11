#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

ROOT=$(nr_root)
TAG=
INPUT=$ROOT/dist
SECOND_INPUT=
OUTPUT=
BUILD=0

usage() {
  cat <<'EOF'
Usage: scripts/package-release.sh --tag <vX.Y.Z[-prerelease]> [options]

Options:
  --input-dir PATH          Directory containing the eight tagged UF2 files.
  --verify-reproducible DIR Compare every UF2 byte-for-byte with a second build.
  --output-dir PATH         Package destination (default: dist/release/<tag>).
  --build                   Build all profiles in the pinned container first.

The command is atomic: a missing or mismatched profile or companion binary
prevents checksums and the build manifest from being published.
EOF
}

while (($#)); do
  case "$1" in
    --tag)
      (($# >= 2)) || nr_die "--tag requires a value"
      TAG=$2
      shift
      ;;
    --input-dir)
      (($# >= 2)) || nr_die "--input-dir requires a path"
      INPUT=$2
      shift
      ;;
    --verify-reproducible)
      (($# >= 2)) || nr_die "--verify-reproducible requires a directory"
      SECOND_INPUT=$2
      shift
      ;;
    --output-dir)
      (($# >= 2)) || nr_die "--output-dir requires a path"
      OUTPUT=$2
      shift
      ;;
    --build) BUILD=1 ;;
    -h|--help) usage; exit 0 ;;
    *) nr_die "unknown option: $1" ;;
  esac
  shift
done

[[ -n $TAG ]] || nr_die "--tag is required"
[[ $TAG =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$ ]] || \
  nr_die "release tag must be SemVer with a leading v"
"$SCRIPT_DIR/check-profiles.py" --release-tag "$TAG"
OUTPUT=${OUTPUT:-$ROOT/dist/release/$TAG}
STAGING=${OUTPUT}.tmp.$$
[[ ! -e $OUTPUT ]] || nr_die "release output already exists; refusing to replace it: ${OUTPUT}"
rm -rf "$STAGING"
cleanup_staging() {
  rm -rf "$STAGING"
}
trap cleanup_staging EXIT

TAG_COMMIT=$(git -C "$ROOT" rev-parse -q --verify "refs/tags/${TAG}^{commit}" 2>/dev/null || true)
[[ -n $TAG_COMMIT ]] || nr_die "tag does not exist locally: $TAG"
[[ $TAG_COMMIT == $(git -C "$ROOT" rev-parse HEAD) ]] || \
  nr_die "release packages must be generated from the exact tag commit"
[[ -z $(git -C "$ROOT" status --porcelain=v1 --untracked-files=all) ]] || \
  nr_die "release packaging requires a clean wrapper and submodules"

if ((BUILD)); then
  NUTTX_RP2350_VERSION=$TAG NUTTX_RP2350_DIST_DIR=$INPUT \
    "$SCRIPT_DIR/build-all.sh" --container --clean
fi

mapfile_supported=0
if builtin help mapfile >/dev/null 2>&1; then
  mapfile_supported=1
fi
PROFILES=()
if ((mapfile_supported)); then
  mapfile -t PROFILES < <(nr_profile_ids "$ROOT")
else
  while IFS= read -r profile; do PROFILES+=("$profile"); done < <(nr_profile_ids "$ROOT")
fi
[[ ${#PROFILES[@]} == 8 ]] || nr_die "atomic release requires exactly eight profiles"

EXPECTED=()
for profile in "${PROFILES[@]}"; do
  slug=$(nr_profile_field "$ROOT" "$profile" artifact_slug)
  name=$(nr_artifact_name "$TAG" "$slug")
  [[ -f $INPUT/$name ]] || nr_die "missing release artifact: ${INPUT}/${name}"
  [[ $name != *-dev-bootsel.uf2 ]] || nr_die "development BOOTSEL image cannot be released"
  EXPECTED+=("$name")
  if [[ -n $SECOND_INPUT ]]; then
    [[ -f $SECOND_INPUT/$name ]] || nr_die "second build is missing ${name}"
    cmp -s "$INPUT/$name" "$SECOND_INPUT/$name" || \
      nr_die "reproducibility failure: ${name} differs between clean builds"
  fi
done

mkdir -p "$STAGING"
for name in "${EXPECTED[@]}"; do
  install -m 0644 "$INPUT/$name" "$STAGING/$name"
done

"$SCRIPT_DIR/bootstrap.sh" --with-release-deps >/dev/null
ESP_SOURCE_REL=$(nr_json_value "$ROOT/sources.lock.json" dependencies.esp_hosted_mcu.path)
ESP_SOURCE=$ROOT/$ESP_SOURCE_REL
ESP_EXPECTED_HASH=$(nr_json_value "$ROOT/sources.lock.json" dependencies.esp_hosted_mcu.sha256)
[[ -f $ESP_SOURCE ]] || nr_die "missing ESP32-C6 release dependency: ${ESP_SOURCE_REL}"
[[ $(nr_sha256 "$ESP_SOURCE") == "$ESP_EXPECTED_HASH" ]] || \
  nr_die "ESP32-C6 release dependency failed checksum validation"
ESP_NAME=nuttx-rp2350-${TAG}-esp32c6-esp-hosted-mcu.bin
install -m 0644 "$ESP_SOURCE" "$STAGING/$ESP_NAME"

MANIFEST_NAME=nuttx-rp2350-${TAG}-build-manifest.json
CHECKSUM_NAME=nuttx-rp2350-${TAG}-SHA256SUMS.txt
SOURCE_DATE_EPOCH=$(nr_source_date_epoch "$ROOT")
NUTTX_COMMIT=$(nr_lock_commit "$ROOT" nuttx)
APPS_COMMIT=$(nr_lock_commit "$ROOT" apps)
PATCH_NUTTX_HASH=$(nr_hash_tree "$ROOT/patches/nuttx")
PATCH_APPS_HASH=$(nr_hash_tree "$ROOT/patches/apps")
OVERLAY_NUTTX_HASH=$(nr_hash_tree "$ROOT/overlays/nuttx")
OVERLAY_APPS_HASH=$(nr_hash_tree "$ROOT/overlays/apps")
CONTAINER=$(nr_container_ref "$ROOT")
TOOLCHAIN=$(nr_json_value "$ROOT/sources.lock.json" build.toolchain 2>/dev/null || printf 'unknown\n')
PROFILES_MANIFEST_HASH=$(nr_sha256 "$ROOT/profiles/manifest.json")
SOURCES_LOCK_HASH=$(nr_sha256 "$ROOT/sources.lock.json")

nr_python - "$ROOT" "$STAGING" "$TAG" "$TAG_COMMIT" "$SOURCE_DATE_EPOCH" \
  "$NUTTX_COMMIT" "$APPS_COMMIT" "$PATCH_NUTTX_HASH" "$PATCH_APPS_HASH" \
  "$OVERLAY_NUTTX_HASH" "$OVERLAY_APPS_HASH" "$CONTAINER" "$TOOLCHAIN" \
  "$PROFILES_MANIFEST_HASH" "$SOURCES_LOCK_HASH" "$MANIFEST_NAME" "$ESP_NAME" \
  "${EXPECTED[@]}" <<'PY'
import hashlib
import json
import os
import sys

(
    root,
    output,
    tag,
    wrapper,
    epoch,
    nuttx,
    apps,
    patch_nuttx,
    patch_apps,
    overlay_nuttx,
    overlay_apps,
    container,
    toolchain,
    profiles_manifest_hash,
    sources_lock_hash,
    manifest_name,
    esp_name,
    *uf2_names,
) = sys.argv[1:]

with open(os.path.join(root, "sources.lock.json"), "r", encoding="utf-8") as stream:
    lock = json.load(stream)
with open(os.path.join(root, "profiles", "manifest.json"), "r", encoding="utf-8") as stream:
    profiles = {item["artifact_slug"]: item for item in json.load(stream)["profiles"]}

def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

artifacts = []
for name in uf2_names + [esp_name]:
    path = os.path.join(output, name)
    item = {
        "name": name,
        "size": os.path.getsize(path),
        "sha256": sha256(path),
        "kind": "esp-hosted-mcu" if name == esp_name else "uf2",
    }
    if name != esp_name:
        slug = next(slug for slug in profiles if f"-{slug}.uf2" in name)
        profile = profiles[slug]
        config_path = os.path.join(root, profile["defconfig"])
        item["profile"] = profile["id"]
        item["config_sha256"] = sha256(config_path)
    artifacts.append(item)

document = {
    "schema_version": 1,
    "project": "NuttX RP2350",
    "tag": tag,
    "wrapper_commit": wrapper,
    "source_date_epoch": int(epoch),
    "upstreams": {"nuttx": nuttx, "apps": apps},
    "source_hashes": {
        "profiles_manifest": profiles_manifest_hash,
        "sources_lock": sources_lock_hash,
        "patches": {"nuttx": patch_nuttx, "apps": patch_apps},
        "overlays": {"nuttx": overlay_nuttx, "apps": overlay_apps},
    },
    "build": {"container": container, "toolchain": toolchain},
    "dependencies": lock.get("dependencies", {}),
    "artifacts": artifacts,
}
with open(os.path.join(output, manifest_name), "w", encoding="utf-8") as stream:
    json.dump(document, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY

(
  cd "$STAGING"
  : > "$CHECKSUM_NAME"
  while IFS= read -r name; do
    printf '%s  %s\n' "$(nr_sha256 "$name")" "$name" >> "$CHECKSUM_NAME"
  done < <(find . -maxdepth 1 -type f ! -name "$CHECKSUM_NAME" -exec basename {} \; | LC_ALL=C sort)
)

ACTUAL_COUNT=$(find "$STAGING" -maxdepth 1 -type f | wc -l | tr -d '[:space:]')
[[ $ACTUAL_COUNT == 11 ]] || nr_die "atomic release must contain exactly 11 assets; found ${ACTUAL_COUNT}"
mkdir -p "$(dirname -- "$OUTPUT")"
mv "$STAGING" "$OUTPUT"
trap - EXIT
nr_note "packaged atomic release in ${OUTPUT}"
