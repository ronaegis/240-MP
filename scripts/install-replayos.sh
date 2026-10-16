#!/bin/bash
# Install, update or remove 240-MP on ReplayOS (Raspberry Pi 5). Run on the Pi
# as root:
#
#   curl -fsSL https://github.com/@REPO@/releases/latest/download/install-replayos.sh | bash
#
#   install-replayos.sh [--version vX] [--local bundle.tar.gz] [--uninstall [--keep-settings]]
#
# 240-MP runs as a ReplayOS core: /opt/replay/cores/240mp_libretro.so starts
# the bundle in /opt/240mp (Qt, mpv, yt-dlp... everything ReplayOS lacks).
# ReplayOS picks a core by matching "<section>/" of /opt/replay/cores/cores.cfg
# against the content path, so a [240mp] section and an empty
# /media/sd/roms/240mp/240-MP.lr make it appear as a game. Settings live in
# /media/sd/240mp, which ReplayOS updates leave alone.
#
# Re-run after a ReplayOS update: it may replace /opt/replay and its core list.
set -euo pipefail

REPO="${MP240_REPO:-@REPO@}"     # filled in by the release workflow
OPT=/opt/240mp
CORES=/opt/replay/cores
CFG=$CORES/cores.cfg
ROMS=/media/sd/roms/240mp
FAV="/media/sd/roms/_favorites/240mp@240-MP.lr.fav"
DATA=/media/sd/240mp

version=latest local_bundle="" uninstall=false keep_settings=false
while [ $# -gt 0 ]; do
   case "$1" in
      --version) version="$2"; shift 2 ;;
      --local) local_bundle="$2"; shift 2 ;;
      --uninstall) uninstall=true; shift ;;
      --keep-settings) keep_settings=true; shift ;;
      *) echo "unknown option: $1" >&2; exit 2 ;;
   esac
done

die() { echo "error: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root"
[ -d "$CORES" ] || die "$CORES not found: is this ReplayOS?"

# Removes the [240mp] section and only it, whatever else is in the file
# (blank lines are held back so none is left dangling at the end).
remove_section() {
   awk '/^\[/ { skip = ($0 == "[240mp]") }
        !skip { if ($0 == "") blank++; else { for (; blank > 0; blank--) print ""; print } }' \
      "$CFG" > "$CFG.240mp-tmp" && mv "$CFG.240mp-tmp" "$CFG"
}

if $uninstall; then
   rm -rf "$OPT" "$CORES/240mp_libretro.so" "$ROMS" "$FAV"
   grep -q '^\[240mp\]' "$CFG" && remove_section
   if $keep_settings; then
      echo "240-MP removed (settings kept in $DATA)"
   else
      rm -rf "$DATA"
      echo "240-MP removed"
   fi
   exit 0
fi

work=$(mktemp -d /tmp/240mp-install.XXXXXX)
trap 'rm -rf "$work"' EXIT

if [ -n "$local_bundle" ]; then
   bundle="$local_bundle"
else
   # Still the placeholder (a copy from the source tree, not from a release);
   # tested by its "@" because the release workflow replaces every "@REPO@".
   case "$REPO" in *@*) die "no repository set (MP240_REPO=owner/240-MP)" ;; esac
   if [ "$version" = latest ]; then
      base="https://github.com/$REPO/releases/latest/download"
   else
      base="https://github.com/$REPO/releases/download/$version"
   fi
   echo "Downloading 240-MP ($version) from $REPO..."
   curl -fsSL -o "$work/SHA256SUMS-replayos" "$base/SHA256SUMS-replayos" ||
      die "no ReplayOS build in release $version of $REPO"
   name=$(awk '/-replayos-arm64\.tar\.gz$/ { print $2; exit }' "$work/SHA256SUMS-replayos")
   [ -n "$name" ] || die "release has no ReplayOS bundle"
   curl -fL --progress-bar -o "$work/$name" "$base/$name"
   (cd "$work" && grep " $name\$" SHA256SUMS-replayos | sha256sum -c --quiet) ||
      die "checksum mismatch for $name"
   bundle="$work/$name"
fi

echo "Installing to $OPT..."
# Unpack beside the old install and swap directories, and rename the core
# into place: a running session keeps the files it has open.
rm -rf /opt/.240mp-new && mkdir /opt/.240mp-new
tar -C /opt/.240mp-new -xzf "$bundle"
[ -x /opt/.240mp-new/240mp/bin/240mp ] && [ -f /opt/.240mp-new/240mp/core/240mp_libretro.so ] ||
   die "not a 240-MP ReplayOS bundle: $bundle"

# Every bundled binary must resolve all its libraries on this system, through
# its own RPATH as at runtime (no LD_LIBRARY_PATH). A ReplayOS update that
# drops a library the bundle relies on shows up here, not as a blank screen.
missing=$(find /opt/.240mp-new/240mp/bin /opt/.240mp-new/240mp/lib /opt/.240mp-new/240mp/plugins \
               /opt/.240mp-new/240mp/qml -type f |
          while read -r f; do
             ldd "$f" 2>/dev/null | grep "not found" | sed "s|^|${f#/opt/.240mp-new}: |" || true
          done)
if [ -n "$missing" ]; then
   rm -rf /opt/.240mp-new
   die "libraries missing on this system:"$'\n'"$missing"
fi

[ -d "$OPT" ] && mv "$OPT" /opt/.240mp-old
mv /opt/.240mp-new/240mp "$OPT" && rm -rf /opt/.240mp-new /opt/.240mp-old
cp "$OPT/core/240mp_libretro.so" "$CORES/.240mp_libretro.so.new"
mv "$CORES/.240mp_libretro.so.new" "$CORES/240mp_libretro.so"

grep -q '^\[240mp\]' "$CFG" ||
   printf '\n[240mp]\nlow = "240mp_libretro.so"\nmid = "240mp_libretro.so"\nhi = "240mp_libretro.so"\n' >> "$CFG"
mkdir -p "$ROMS" "$DATA" "$(dirname "$FAV")"
: > "$ROMS/240-MP.lr"
printf '/roms/240mp/240-MP.lr' > "$FAV"

echo "240-MP $(cat "$OPT/VERSION" 2>/dev/null || echo "") installed: Favorites -> 240-MP"
