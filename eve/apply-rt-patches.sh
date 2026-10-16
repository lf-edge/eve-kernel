#!/bin/sh
# Apply (or revert) the PREEMPT_RT patch series.
#
# Upstream ships RT two ways: one rolled-up patch file, and a tarball of the
# individual patches plus a `series` file giving their order. We take the
# series form so that patches for architectures this branch does not build
# can be left out.
#
# That is not just tidiness. RT trails the stable line by several point
# releases, and the patches most likely to go stale are the ones fixing
# arch code we never compile - as of 6.18.55, arch/arm/mm/fault.c had
# already been fixed upstream, so the rolled-up patch exited non-zero and
# failed the build over a file amd64 never sees.
#
# A patch is skipped when EVERY file it touches is under an architecture in
# $SKIP_ARCHES. A patch touching one of those plus anything else is still
# applied. Deciding from the patch's own contents means no hand-maintained
# skip list to update on the next RT bump.
#
# Usage: apply-rt-patches.sh <series-tarball> [--revert]
#
# <series-tarball> is a local file, or a URL to fetch it from.

set -eu

SRC=${1:?usage: apply-rt-patches.sh <series-tarball> [--revert]}
MODE=${2:-apply}

SKIP_ARCHES='arm|arm64|powerpc|riscv|s390|mips|loongarch|sparc|xtensa|openrisc|parisc|alpha|m68k|sh|csky|hexagon|nios2|microblaze'

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

if [ -f "$SRC" ]; then
    tar xzf "$SRC" -C "$WORKDIR"
else
    echo "Fetching RT series from $SRC"
    curl -fsSL "$SRC" | tar xz -C "$WORKDIR"
fi

PDIR="$WORKDIR/patches"
[ -f "$PDIR/series" ] || { echo "ERROR: no patches/series in $SRC" >&2; exit 1; }

# series order, comments and blank lines stripped
SERIES=$(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' -e 's/[[:space:]]*$//' "$PDIR/series")
[ -n "$SERIES" ] || { echo "ERROR: series file lists no patches" >&2; exit 1; }

# A patch is for another architecture only if no file it touches lies
# outside $SKIP_ARCHES.
is_other_arch_only() {
    ! grep -E '^\+\+\+ b/' "$1" \
        | sed 's|^+++ b/||' \
        | grep -qvE "^arch/($SKIP_ARCHES)/"
}

if [ "$MODE" = "--revert" ]; then
    # reverse order, reverse direction; a failure here is a warning, since
    # the tree may have been only partly patched
    SERIES=$(printf '%s\n' "$SERIES" | sed '1!G;h;$!d')   # reverse, without tac
    PATCHARGS="-R -N"
    VERB="Reverting"
else
    PATCHARGS="-N"
    VERB="Applying"
fi

# Drive the loop from a file, not a pipe: a piped `while` runs in a subshell,
# where the counters would be lost and a failure could not abort the script.
printf '%s\n' "$SERIES" > "$WORKDIR/order"

applied=0
skipped=0
while IFS= read -r name; do
    f="$PDIR/$name"
    if [ ! -f "$f" ]; then
        echo "ERROR: '$name' is in series but not in the tarball" >&2
        exit 1
    fi
    if is_other_arch_only "$f"; then
        echo "  skip (other architectures only): $name"
        skipped=$((skipped + 1))
        continue
    fi
    echo "  $VERB $name"
    if ! patch -p1 $PATCHARGS --no-backup-if-mismatch < "$f"; then
        if [ "$MODE" = "--revert" ]; then
            echo "WARNING: could not cleanly revert $name; check git status" >&2
        else
            echo "ERROR: failed to apply $name" >&2
            exit 1
        fi
    fi
    applied=$((applied + 1))
done < "$WORKDIR/order"

echo "RT series: $applied patches $([ "$MODE" = --revert ] && echo reverted || echo applied), $skipped skipped as other-architecture."
