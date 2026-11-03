#!/usr/bin/env bash
# Eglantine for Steam Deck launcher.
#
# Runs the AppImage next to this script with the settings file next to this script, the
# environment the fork is developed with, and the per-game switches that are known to help.
#
#   eglantine-deck.sh <game folder or path to exefs/main> [eden-cli options...]
#
# Examples (a loose dump at ~/Emulation/roms/switch2/<game>/{exefs,romfs}):
#   eglantine-deck.sh dk-bananza
#   eglantine-deck.sh ~/Emulation/roms/switch2/mkw
#   eglantine-deck.sh mkw --windowed
#
# Steam shortcut (Game Mode): add this script as a non-Steam game and set the launch options to
# the game folder name, e.g.  dk-bananza
#
# Switches (also as environment variables, which win over the switch):
#   --frames-ahead   EDEN_NX2_FRAMES_AHEAD=a  let the game run one frame ahead of the GPU thread.
#                    Mario Kart World: 18 -> 25 fps in the race. Donkey Kong Bananza: FREEZES at
#                    big terrain breaks; the script refuses to combine them.
#   --chunk-batch    EDEN_NX2_CHUNK_BATCH=1   draw Donkey Kong Bananza's terrain chunks in
#                    batches: +1-2 fps in the opening cave, no known downside. On by default for
#                    a folder whose name contains "dk" or "bananza".
#   --no-mesh        EDEN_NX2_NO_MESH=1       skip task/mesh shaders (Mario Kart World's trail
#                    ribbons and splats): +2 fps in the race, less correct picture.
#   --windowed       no fullscreen.
#   --appimage FILE  use this AppImage instead of the newest one next to the script.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
roms="${EGLANTINE_ROMS:-$HOME/Emulation/roms/switch2}"
settings="${EGLANTINE_SETTINGS:-$here/eglantine-deck.ini}"

die() { echo "eglantine-deck: $*" >&2; exit 1; }

appimage=""
fullscreen=-f
game=""
frames_ahead=""
chunk_batch=""
no_mesh=""
extra=()
while [ $# -gt 0 ]; do
    case "$1" in
    --frames-ahead) frames_ahead=a ;;
    --chunk-batch) chunk_batch=1 ;;
    --no-mesh) no_mesh=1 ;;
    --windowed) fullscreen="" ;;
    --appimage) shift; appimage="${1:-}" ;;
    -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --) shift; [ $# -gt 0 ] && extra+=("$@"); break ;;
    -*) extra+=("$1") ;;
    *) [ -z "$game" ] && game="$1" || extra+=("$1") ;;
    esac
    shift
done
[ -n "$game" ] || die "which game? usage: $(basename "$0") <game folder> [--frames-ahead] [--chunk-batch] [--no-mesh]"

# The game: a folder name under $roms, a folder, or the exefs/main file itself.
if [ -f "$game" ]; then
    main="$game"
elif [ -d "$game" ]; then
    main="$game/exefs/main"
elif [ -d "$roms/$game" ]; then
    main="$roms/$game/exefs/main"
else
    die "no game folder '$game' (looked in $roms); a loose dump is a folder with exefs/ and romfs/"
fi
[ -f "$main" ] || die "no exefs/main under $(dirname "$(dirname "$main")"); is this a loose dump?"
title="$(basename "$(dirname "$(dirname "$main")")")"

# The AppImage: given, or the newest eglantine-steamdeck-*.AppImage next to the script, or in
# ~/Applications.
if [ -z "$appimage" ]; then
    for dir in "$here" "$HOME/Applications"; do
        appimage="$(ls -t "$dir"/eglantine-steamdeck-*.AppImage 2>/dev/null | head -1)"
        [ -n "$appimage" ] && break
    done
fi
[ -n "$appimage" ] && [ -f "$appimage" ] || die "no eglantine-steamdeck-*.AppImage next to this script or in ~/Applications"
[ -x "$appimage" ] || chmod +x "$appimage" || die "cannot make $appimage executable"
[ -f "$settings" ] || die "settings file $settings is missing; it comes with this script"

# Per-game switches. Environment variables already set win.
case "$title" in
*dk*|*bananza*|*DK*|*Bananza*)
    : "${chunk_batch:=1}"
    if [ "${EDEN_NX2_FRAMES_AHEAD:-$frames_ahead}" = a ]; then
        die "EDEN_NX2_FRAMES_AHEAD=a freezes Donkey Kong Bananza at terrain breaks; not setting it"
    fi
    ;;
esac
export EDEN_NX2_AUTO_STUB="${EDEN_NX2_AUTO_STUB:-1}"
[ -n "$frames_ahead" ] && export EDEN_NX2_FRAMES_AHEAD="${EDEN_NX2_FRAMES_AHEAD:-$frames_ahead}"
[ -n "$chunk_batch" ] && export EDEN_NX2_CHUNK_BATCH="${EDEN_NX2_CHUNK_BATCH:-$chunk_batch}"
[ -n "$no_mesh" ] && export EDEN_NX2_NO_MESH="${EDEN_NX2_NO_MESH:-$no_mesh}"

# A loose dump opens thousands of files at once; the default limit of 1024 gives "Too many
# open files".
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

echo "eglantine-deck: $title, $(basename "$appimage"), settings $(basename "$settings")$(env | grep '^EDEN_NX2_' | sed 's/^/, /' | tr -d '\n')" >&2
exec "$appimage" $fullscreen -c "$settings" -g "$main" ${extra[@]+"${extra[@]}"}
