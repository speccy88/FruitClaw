#!/bin/sh
#
# SPDX-License-Identifier: Apache-2.0
#
# Canonicalize archive member order after parallel nuttx-apps builds.  The
# per-application flock prevents corruption, but lock acquisition order is
# nondeterministic and otherwise changes the final firmware link layout.

set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: canonicalize_archive.sh <ar-tool> <archive>" >&2
  exit 2
fi

ar_tool=$1
archive=$2
member_list=${archive}.members.$$
actual_list=${archive}.actual.$$

cleanup()
{
  rm -f "$member_list" "$actual_list"
}

trap cleanup EXIT HUP INT TERM

if [ ! -f "$archive" ]; then
  echo "canonicalize_archive.sh: archive not found: $archive" >&2
  exit 1
fi

list_members()
{
  # GNU ar hides its index from `ar t`; BSD ar reports it as
  # "__.SYMDEF" or "__.SYMDEF SORTED".  It is metadata, not an object.
  "$ar_tool" t "$archive" | sed '/^__.SYMDEF/d'
}

list_members | LC_ALL=C sort > "$member_list"
[ -s "$member_list" ] || exit 0

if uniq -d "$member_list" | grep -q .; then
  echo "canonicalize_archive.sh: duplicate archive member names are unsupported" >&2
  exit 1
fi

# Move every member to the end in sorted batches.  Once all batches have
# moved, the complete archive is ordered exactly like member_list without
# extracting or rewriting any object files.
xargs -n 100 "$ar_tool" m "$archive" < "$member_list"
"$ar_tool" s "$archive"

list_members > "$actual_list"
if ! cmp -s "$member_list" "$actual_list"; then
  echo "canonicalize_archive.sh: failed to produce sorted member order" >&2
  exit 1
fi
