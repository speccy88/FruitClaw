#!/usr/bin/env bash

# Shared helpers for the NuttX RP2350 wrapper scripts.  This file deliberately
# has no `set` command so callers can choose their own shell strictness.

nr_die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

nr_note() {
  printf '==> %s\n' "$*"
}

nr_root() {
  if [[ -n ${NUTTX_RP2350_ROOT:-} ]]; then
    (CDPATH='' cd -- "$NUTTX_RP2350_ROOT" && pwd)
  else
    (CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
  fi
}

nr_require_tool() {
  command -v "$1" >/dev/null 2>&1 || nr_die "required tool not found: $1"
}

nr_python() {
  nr_require_tool python3
  python3 "$@"
}

nr_json_value() {
  local file=$1
  shift

  nr_python - "$file" "$@" <<'PY'
import json
import sys

path = sys.argv[1]
candidates = sys.argv[2:]
with open(path, "r", encoding="utf-8") as stream:
    document = json.load(stream)

for candidate in candidates:
    value = document
    try:
        for component in candidate.split("."):
            value = value[int(component)] if isinstance(value, list) else value[component]
    except (KeyError, IndexError, TypeError, ValueError):
        continue
    if value is None:
        continue
    if isinstance(value, bool):
        print("true" if value else "false")
    elif isinstance(value, (dict, list)):
        print(json.dumps(value, separators=(",", ":")))
    else:
        print(value)
    sys.exit(0)

sys.exit(1)
PY
}

nr_lock_commit() {
  local root=$1
  local source=$2
  nr_json_value "$root/sources.lock.json" \
    "upstreams.${source}.commit" \
    "sources.${source}.commit" \
    "${source}.commit" \
    "${source}_commit" || \
    nr_die "sources.lock.json does not contain a commit for ${source}"
}

nr_lock_url() {
  local root=$1
  local source=$2
  local fallback

  case "$source" in
    nuttx) fallback=https://github.com/apache/nuttx.git ;;
    apps) fallback=https://github.com/apache/nuttx-apps.git ;;
    *) nr_die "unknown source: $source" ;;
  esac

  nr_json_value "$root/sources.lock.json" \
    "upstreams.${source}.url" \
    "sources.${source}.url" \
    "${source}.url" 2>/dev/null || printf '%s\n' "$fallback"
}

nr_profile_field() {
  local root=$1
  local profile=$2
  local field=$3

  nr_python - "$root/profiles/manifest.json" "$profile" "$field" <<'PY'
import json
import sys

manifest_path, profile_id, field = sys.argv[1:]
with open(manifest_path, "r", encoding="utf-8") as stream:
    manifest = json.load(stream)

for profile in manifest.get("profiles", []):
    if profile.get("id") == profile_id:
        value = profile
        try:
            for component in field.split("."):
                value = value[component]
        except (KeyError, TypeError):
            sys.exit(1)
        if isinstance(value, bool):
            print("true" if value else "false")
        elif isinstance(value, (dict, list)):
            print(json.dumps(value, separators=(",", ":")))
        else:
            print(value)
        sys.exit(0)

sys.exit(2)
PY
}

nr_profile_ids() {
  local root=$1
  nr_python - "$root/profiles/manifest.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    manifest = json.load(stream)
for profile in manifest.get("profiles", []):
    print(profile["id"])
PY
}

nr_gitlink_commit() {
  local root=$1
  local path=$2
  git -C "$root" ls-files --stage -- "$path" | awk '$1 == "160000" { print $2; exit }'
}

nr_wrapper_state() {
  local root=$1

  # Do not use `git status` here.  Refreshing a gitlink makes Git open the
  # nested repository even with --ignore-submodules=all; on cloud-backed
  # workspaces that metadata read can block for minutes.  Snapshot the wrapper
  # index and ordinary tracked-file contents without entering either upstream
  # submodule.  Do not inventory untracked paths here: on cloud-backed
  # workspaces `git ls-files --others` can block for minutes, and it did not
  # hash untracked contents anyway.  Build outputs are confined to explicit
  # work and distribution directories by their callers.

  git -C "$root" ls-files --stage | sed 's/^/I /'
  paste -d ' ' \
    <(git -C "$root" ls-files --stage | \
      awk '$1 != "160000" {sub(/^[^\t]*\t/, ""); print}') \
    <(git -C "$root" ls-files --stage | \
      awk '$1 != "160000" {sub(/^[^\t]*\t/, ""); print}' | \
      git -C "$root" hash-object --stdin-paths) | sed 's/^/W /'
}

nr_ensure_commit() {
  local root=$1
  local source=$2
  local commit=$3
  local url=$4
  local repo=$root/$source

  nr_require_tool git

  if ! git -C "$repo" rev-parse --git-dir >/dev/null 2>&1; then
    nr_note "initializing ${source} submodule"
    git -C "$root" submodule update --init --recursive --checkout -- "$source"
  fi

  if ! git -C "$repo" cat-file -e "${commit}^{commit}" 2>/dev/null; then
    nr_note "fetching pinned ${source} commit ${commit}"
    git -C "$repo" fetch --no-tags "$url" "$commit"
  fi

  git -C "$repo" cat-file -e "${commit}^{commit}" 2>/dev/null || \
    nr_die "${source} commit is unavailable after fetch: ${commit}"
}

nr_prepare_source_cache() {
  local cache=$1
  local source=$2
  local commit=$3
  local url=$4
  local fetch_from=$url
  local local_repo

  nr_require_tool git

  if [[ ! -d $cache ]]; then
    nr_note "initializing immutable ${source} source cache"
    mkdir -p "$(dirname -- "$cache")"
    git init --bare -q "$cache"
  fi

  [[ $(git --git-dir="$cache" rev-parse --is-bare-repository 2>/dev/null) == true ]] || \
    nr_die "invalid ${source} source cache: ${cache}"

  # The common case is an already-populated immutable cache.  Check it before
  # opening a submodule working tree: on cloud-backed macOS workspaces merely
  # reading nested Git metadata can block for minutes.  CI has local checked-
  # out submodules, so only there do we use their object stores to avoid a
  # redundant network fetch when the cache is cold.

  if git --git-dir="$cache" cat-file -e "${commit}^{commit}" 2>/dev/null; then
    return 0
  fi

  if [[ ${CI:-} == true ]]; then
    local_repo=$(nr_root)/$source
    if git -C "$local_repo" cat-file -e "${commit}^{commit}" 2>/dev/null; then
      fetch_from=$local_repo
    fi
  fi

  if ! git --git-dir="$cache" cat-file -e "${commit}^{commit}" 2>/dev/null; then
    nr_note "fetching pinned ${source} source ${commit}"
    git --git-dir="$cache" fetch --no-tags --depth=1 "$fetch_from" "$commit"
  fi

  git --git-dir="$cache" cat-file -e "${commit}^{commit}" 2>/dev/null || \
    nr_die "${source} commit is unavailable in source cache: ${commit}"
}

nr_materialize_commit() {
  local repo=$1
  local commit=$2
  local destination=$3

  nr_require_tool git
  nr_require_tool tar
  mkdir -p "$destination"
  git -C "$repo" archive --format=tar "$commit" | tar -xf - -C "$destination"
  # Isolate `git apply` from the wrapper repository above build/.  Without a
  # nested repository, Git discovers the wrapper root and can return success
  # while saying "Skipped patch" for every staged-source path.
  git init -q "$destination"
}

nr_apply_series() {
  local label=$1
  local destination=$2
  local patch_directory=$3
  local series=$patch_directory/series
  local entry
  local patch
  local count=0
  local output

  [[ -f $series ]] || nr_die "missing ${label} patch series: ${series}"

  while IFS= read -r entry || [[ -n $entry ]]; do
    entry=${entry%%#*}
    entry=${entry#"${entry%%[![:space:]]*}"}
    entry=${entry%"${entry##*[![:space:]]}"}
    [[ -n $entry ]] || continue
    [[ $entry != /* && $entry != *../* && $entry != ../* ]] || \
      nr_die "unsafe entry in ${series}: ${entry}"
    patch=$patch_directory/$entry
    [[ -f $patch ]] || nr_die "${series} references missing patch: ${entry}"
    nr_note "applying ${label} patch ${entry}"
    if ! output=$(git -C "$destination" apply --check --verbose --whitespace=nowarn "$patch" 2>&1); then
      printf '%s\n' "$output" >&2
      nr_die "${label} patch does not apply: ${entry}"
    fi
    if grep -q 'Skipped patch' <<< "$output"; then
      printf '%s\n' "$output" >&2
      nr_die "Git skipped paths while checking ${label} patch: ${entry}"
    fi
    if ! output=$(git -C "$destination" apply --verbose --whitespace=nowarn "$patch" 2>&1); then
      printf '%s\n' "$output" >&2
      nr_die "failed to apply ${label} patch: ${entry}"
    fi
    if grep -q 'Skipped patch' <<< "$output"; then
      printf '%s\n' "$output" >&2
      nr_die "Git skipped paths while applying ${label} patch: ${entry}"
    fi
    count=$((count + 1))
  done < "$series"

  nr_note "applied ${count} ${label} patch(es)"
}

nr_copy_overlay() {
  local label=$1
  local overlay=$2
  local destination=$3

  [[ -d $overlay ]] || nr_die "missing ${label} overlay directory: ${overlay}"
  if find "$overlay" -mindepth 1 -print -quit | grep -q .; then
    nr_note "installing ${label} overlay"
    tar -cf - -C "$overlay" . | tar -xf - -C "$destination"
  else
    nr_note "${label} overlay is empty"
  fi
}

nr_sha256() {
  local path=$1
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$path" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$path" | awk '{print $1}'
  else
    nr_python - "$path" <<'PY'
import hashlib
import sys

digest = hashlib.sha256()
with open(sys.argv[1], "rb") as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(block)
print(digest.hexdigest())
PY
  fi
}

nr_download_verified() {
  local url=$1
  local expected=$2
  local destination=$3
  local actual
  local temporary=${destination}.download.$$

  mkdir -p "$(dirname -- "$destination")"
  if [[ -f $destination ]]; then
    actual=$(nr_sha256 "$destination")
    if [[ $actual == "$expected" ]]; then
      return 0
    fi
    rm -f "$destination"
  fi

  rm -f "$temporary"
  if command -v curl >/dev/null 2>&1; then
    curl --fail --location --retry 3 --output "$temporary" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "$temporary" "$url"
  else
    nr_die "curl or wget is required to download ${url}"
  fi
  actual=$(nr_sha256 "$temporary")
  if [[ $actual != "$expected" ]]; then
    rm -f "$temporary"
    nr_die "download checksum mismatch for ${url}: expected ${expected}, got ${actual}"
  fi
  mv "$temporary" "$destination"
}

nr_hash_tree() {
  local path=$1
  nr_python - "$path" <<'PY'
import hashlib
import os
import stat
import sys

root = os.path.abspath(sys.argv[1])
digest = hashlib.sha256()
if not os.path.isdir(root):
    print(digest.hexdigest())
    sys.exit(0)

for current, directories, filenames in os.walk(root):
    directories.sort()
    filenames.sort()
    for name in filenames:
        path = os.path.join(current, name)
        relative = os.path.relpath(path, root).replace(os.sep, "/")
        metadata = os.lstat(path)
        digest.update(relative.encode("utf-8") + b"\0")
        digest.update((b"x" if metadata.st_mode & stat.S_IXUSR else b"-") + b"\0")
        if stat.S_ISLNK(metadata.st_mode):
            digest.update(b"link\0" + os.readlink(path).encode("utf-8"))
        else:
            digest.update(b"file\0")
            with open(path, "rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
        digest.update(b"\0")
print(digest.hexdigest())
PY
}

nr_source_date_epoch() {
  local root=$1
  local configured

  configured=$(nr_json_value "$root/sources.lock.json" \
    build.source_date_epoch reproducibility.source_date_epoch 2>/dev/null || true)
  if [[ $configured =~ ^[0-9]+$ ]]; then
    printf '%s\n' "$configured"
  else
    git -C "$root" show -s --format=%ct HEAD 2>/dev/null || printf '0\n'
  fi
}

nr_container_ref() {
  local root=$1
  local direct
  local image
  local digest

  direct=$(nr_json_value "$root/sources.lock.json" \
    build.container_ref toolchain.container_ref container.ref 2>/dev/null || true)
  if [[ -n $direct ]]; then
    printf '%s\n' "$direct"
    return
  fi

  image=$(nr_json_value "$root/sources.lock.json" \
    build.container_image toolchain.container_image container.image 2>/dev/null || true)
  digest=$(nr_json_value "$root/sources.lock.json" \
    build.container_digest toolchain.container_digest container.digest 2>/dev/null || true)
  [[ -n $image && -n $digest ]] || \
    nr_die "sources.lock.json must pin a build container by digest"
  digest=${digest#@}
  [[ $digest == sha256:* ]] || digest=sha256:$digest
  printf '%s@%s\n' "$image" "$digest"
}

nr_config_set() {
  local config=$1
  local symbol=$2
  local value=$3
  local temporary=${config}.tmp.$$

  awk -v symbol="$symbol" \
    '$0 !~ "^CONFIG_" symbol "=" && $0 !~ "^# CONFIG_" symbol " is not set$"' \
    "$config" > "$temporary"
  printf 'CONFIG_%s=%s\n' "$symbol" "$value" >> "$temporary"
  mv "$temporary" "$config"
}

nr_config_disable() {
  local config=$1
  local symbol=$2
  local temporary=${config}.tmp.$$

  awk -v symbol="$symbol" \
    '$0 !~ "^CONFIG_" symbol "=" && $0 !~ "^# CONFIG_" symbol " is not set$"' \
    "$config" > "$temporary"
  printf '# CONFIG_%s is not set\n' "$symbol" >> "$temporary"
  mv "$temporary" "$config"
}

nr_config_symbol_known() {
  local tree=$1
  local symbol=$2
  grep -R -E -q "^[[:space:]]*(menu)?config[[:space:]]+${symbol}([[:space:]]|$)" \
    "$tree/nuttx" "$tree/apps" 2>/dev/null
}

nr_normalize_host_config() {
  local config=$1
  local host=$2

  case "$host" in
    linux)
      nr_config_set "$config" HOST_LINUX y
      nr_config_disable "$config" HOST_MACOS
      nr_config_disable "$config" HOST_WINDOWS
      nr_config_disable "$config" HOST_OTHER
      ;;
    macos)
      nr_config_set "$config" HOST_MACOS y
      nr_config_disable "$config" HOST_LINUX
      nr_config_disable "$config" HOST_WINDOWS
      nr_config_disable "$config" HOST_OTHER
      ;;
    *) nr_die "unsupported build host: ${host}" ;;
  esac
}

nr_artifact_name() {
  local version=$1
  local slug=$2
  local suffix=${3:-}
  printf 'nuttx-rp2350-%s-%s%s.uf2\n' "$version" "$slug" "$suffix"
}
