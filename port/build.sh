#!/usr/bin/env bash
# Build Metal Gear Solid for the ESP32-S3 (Xtensa), against PSY-Z instead of the
# PSY-Q SDK.
#
# Everything lands in $BUILD (default: build-xtensa/ inside the repo) so the
# objects survive between sessions -- an earlier attempt used /tmp and lost the
# whole tree every time the shell restarted.
#
#   ./port/build.sh            incremental
#   ./port/build.sh clean      from scratch
#   ./port/build.sh link       relink only, print what is still unresolved
#
# Requires: xtensa-esp32s3-elf-gcc on PATH or XT set, and a sotn-decomp checkout
# (for PSY-Z and the PSY-Q decomp) pointed at by SOTN.

set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-$REPO/build-xtensa}"
SOTN="${SOTN:-/e/Hardware/sotn/sotn-decomp}"
PSYZ="$SOTN/tools/psyz/psyz"
PSYQ="$SOTN/tools/psyz/decomp"
SHIM="${SHIM:-/c/Users/David/.claude/skills/retro-port-triage/scripts/shim}"
XT="${XT:-xtensa-esp32s3-elf-gcc}"
AR="${AR:-${XT%gcc}ar}"

# -std=gnu17 is required: GCC 14 defaults to gnu23, which rejects several
# PSY-Q-era constructs this code relies on.
CFLAGS="-c -w -std=gnu17 -O2 -DINTEGRAL -D__psyz"
# PSY-Q code is full of int/long and pointer mismatches that were harmless on
# MIPS, where the two are the same width. Same precedent as SOTN's esp32 build.
CFLAGS="$CFLAGS -Wno-incompatible-pointer-types -Wno-int-conversion"
CFLAGS="$CFLAGS -Wno-builtin-declaration-mismatch -Wno-implicit-function-declaration"
CFLAGS="$CFLAGS -Wno-return-mismatch"
# Xtensa call8 reaches +/-512 KB; this image is several MB, so every call
# must be a long call. ESP-IDF passes this for the same reason.
CFLAGS="$CFLAGS -mlongcalls"
# The game's own include dirs MUST come before PSY-Z's: both ship a common.h.
INCS="-I$REPO/source -I$REPO/source/include -I$SHIM -I$PSYZ/include"
PRE="-include string.h -include gtemac.h"

# Only ONE stage may be linked: each of the 93 files in source/stage/ defines
# its own _StageCharacterEntries[], because on the PSX one stage is resident at
# a time. Same overlay-collision problem the SOTN port solved by renaming.
STAGE="${STAGE:-s07a}"

case "${1:-build}" in clean) rm -rf "$BUILD" ;; esac
mkdir -p "$BUILD/obj" "$BUILD/lib"

compile() {  # compile <src> <extra-includes...>
    local src="$1"; shift
    local obj="$BUILD/obj/$(echo "${src#$REPO/}" | tr '/' '_' | sed 's/\.c$/.o/')"
    if [ -f "$obj" ] && [ "$obj" -nt "$src" ]; then return 0; fi
    if "$XT" $CFLAGS $INCS "$@" $PRE "$src" -o "$obj" 2>>"$BUILD/errors.log"; then
        return 0
    fi
    echo "$src" >> "$BUILD/failed.log"
    return 1
}

if [ "${1:-build}" != "link" ]; then
    : > "$BUILD/errors.log"; : > "$BUILD/failed.log"
    ok=0; bad=0

    echo "== game =="
    while read -r f; do
        compile "$f" && ok=$((ok+1)) || bad=$((bad+1))
    done < <(find "$REPO/source" -name '*.c' \
                  -not -path '*/overlays/*' -not -path '*/contrib/*' \
                  -not -path '*/stagevr/*' -not -path '*/snake_vr/*' \
                  -not -path '*/stage/*')
    # exactly one stage, plus the shared stage support file
    compile "$REPO/source/stage/_stage.c" && ok=$((ok+1)) || bad=$((bad+1))
    compile "$REPO/source/stage/$STAGE.c" && ok=$((ok+1)) || bad=$((bad+1))

    echo "== overlays that are 100% C =="
    for d in s07a s11e s11i d18ar s08br s19br; do
        for f in "$REPO/source/overlays/$d"/*.c; do
            [ -f "$f" ] && { compile "$f" && ok=$((ok+1)) || bad=$((bad+1)); }
        done
    done

    echo "== port layer =="
    for f in "$REPO"/port/*.c; do
        compile "$f" -I"$PSYZ/src" && ok=$((ok+1)) || bad=$((bad+1))
    done

    echo "== PSY-Z =="
    for f in "$PSYZ"/src/psyz/*.c; do
        # libsn.c provides the PC* host-file API, but MGS ships its own in
        # libfs/select.c and the game's version is the one it expects
        case "$f" in *libgte_hw_sqrt*|*libsn.c) continue;; esac
        "$XT" -c -w -std=gnu17 -O2 -mlongcalls -D__psyz -I"$PSYZ/include" -I"$PSYZ/src" \
              -I"$SOTN/include" "$f" -o "$BUILD/obj/psyz_$(basename "$f" .c).o" \
              2>>"$BUILD/errors.log" && ok=$((ok+1)) || bad=$((bad+1))
    done

    echo "== PSY-Q decomp =="
    for f in "$PSYQ"/src/libgpu/*.c "$PSYQ"/src/libgte/*.c "$PSYQ"/src/libcd/*.c \
             "$PSYQ"/src/libspu/*.c "$PSYQ"/src/libapi/*.c "$PSYQ"/src/libetc/*.c \
             "$PSYQ"/src/libcard/*.c "$PSYQ"/src/libpress/*.c; do
        [ -f "$f" ] || continue
        "$XT" -c -w -std=gnu17 -O2 -mlongcalls -D__psyz -DVERSION_PC -DNON_MATCHING \
              -DPERMUTER -DHARD_LINK -D_internal_version_us \
              -I"$PSYZ/include" -I"$PSYZ/src" -I"$SOTN/include" -include stdio.h \
              "$f" -o "$BUILD/obj/psyq_$(echo "${f#$PSYQ/src/}" | tr '/' '_' | sed 's/\.c$/.o/')" \
              2>>"$BUILD/errors.log" && ok=$((ok+1)) || bad=$((bad+1))
    done

    echo "compiled: $ok   failed: $bad   (see $BUILD/failed.log)"
fi

rm -f "$BUILD/lib/libmgs.a"
ls "$BUILD/obj"/*.o | xargs -n 40 "$AR" rcs "$BUILD/lib/libmgs.a"

"$XT" -o "$BUILD/mgs.elf" -Wl,--whole-archive "$BUILD/lib/libmgs.a" \
      -Wl,--no-whole-archive -nostartfiles -e main > "$BUILD/link.log" 2>&1

und=$(grep -c 'undefined reference' "$BUILD/link.log")
dup=$(grep -c 'multiple definition' "$BUILD/link.log")
echo
echo "undefined: $und    multiple definitions: $dup"
[ "$dup" -gt 0 ] && grep 'multiple definition' "$BUILD/link.log" |
    grep -oE "of \`[^']*'" | sed "s/of .//;s/.\$//" | sort -u | head -20
[ -f "$BUILD/mgs.elf" ] && echo "ELF: $BUILD/mgs.elf"
