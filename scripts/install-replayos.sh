#!/bin/bash
# Install, update or remove 240-MP on ReplayOS (Raspberry Pi 5). Run on the Pi
# as root:
#
#   curl -fsSL https://github.com/@REPO@/releases/latest/download/install-replayos.sh | bash
#
#   install-replayos.sh [--version vX] [--local bundle.tar.gz] [--no-restart]
#                       [--uninstall [--keep-settings]]
#
# 240-MP runs as a ReplayOS external core (ReplayOS 2.1.0 or later,
# https://www.replayos.com/externalcores/): /opt/replay/cores/240mp_libretro.so
# starts the bundle in /opt/240mp (Qt, mpv, yt-dlp... everything ReplayOS
# lacks), and 240mp_libretro.cfg next to it registers the "mp240" system (an
# ID must start with a letter). Its games are in roms/mp240/ on the storage
# picked in ReplayOS (System > Storage: /media/sd, /media/nvme, /media/usb or
# /media/nfs), where an empty 240-MP.lr starts 240-MP. Settings are kept in
# 240mp/ on that storage.
#
# ReplayOS reads registrations when it starts, so this restarts it when the
# registration changes (not for a plain update). OTA updates keep the core;
# run this again after changing the storage.
set -euo pipefail

REPO="${MP240_REPO:-@REPO@}"     # filled in by the release workflow
OPT=/opt/240mp
CORES=/opt/replay/cores
CORE_CFG=$CORES/240mp_libretro.cfg
SYSTEM=mp240
# Before external cores: a [240mp] section in cores.cfg, roms/240mp/ and a
# favorite. Removed when found.
LEGACY_CFG=$CORES/cores.cfg
# Earlier installs raised CMA here; the block is removed when found (the
# bridge now fits HEVC in the stock 64 MB).
BOOT_CFG=/boot/firmware/config.user.txt
CMA_MARK="# 240-MP: more CMA"

version=latest local_bundle="" uninstall=false keep_settings=false restart=true
while [ $# -gt 0 ]; do
   case "$1" in
      --version) version="$2"; shift 2 ;;
      --local) local_bundle="$2"; shift 2 ;;
      --no-restart) restart=false; shift ;;
      --uninstall) uninstall=true; shift ;;
      --keep-settings) keep_settings=true; shift ;;
      *) echo "unknown option: $1" >&2; exit 2 ;;
   esac
done

die() { echo "error: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root"
[ -d "$CORES" ] || die "$CORES not found: is this ReplayOS?"
grep -aq '_libretro\.cfg' /opt/replay/replay ||
   die "this ReplayOS has no external cores: update it to 2.1.0 or later"

# Removes what installs made before external cores, from every storage (it
# may have changed since). The [240mp] section goes and only it, whatever
# else is in cores.cfg (blank lines are held back so none is left dangling).
remove_legacy() {
   if grep -q '^\[240mp\]' "$LEGACY_CFG" 2>/dev/null; then
      awk '/^\[/ { skip = ($0 == "[240mp]") }
           !skip { if ($0 == "") blank++; else { for (; blank > 0; blank--) print ""; print } }' \
         "$LEGACY_CFG" > "$LEGACY_CFG.240mp-tmp" && mv "$LEGACY_CFG.240mp-tmp" "$LEGACY_CFG"
   fi
   for s in /media/*; do
      rm -rf "$s/roms/240mp" "$s"/roms/_favorites/240mp@*.fav
   done
}

# Removes the CMA block earlier installs wrote: its comment line up to its
# closing [all]. The boot settings are back to ReplayOS's after a reboot.
remove_cma() {
   [ -f "$BOOT_CFG" ] && grep -qF "$CMA_MARK" "$BOOT_CFG" || return 0
   awk -v mark="$CMA_MARK" 'index($0, mark) == 1 { skip = 1; next }
        skip { if ($0 == "[all]") skip = 0; next }
        { if ($0 == "") blank++; else { for (; blank > 0; blank--) print ""; print } }' \
      "$BOOT_CFG" > "$BOOT_CFG.240mp-tmp" && mv "$BOOT_CFG.240mp-tmp" "$BOOT_CFG"
   echo "Removed the CMA setting an earlier 240-MP install added to $BOOT_CFG (no longer needed)"
}

restart_replay() {
   if $restart; then
      echo "Restarting ReplayOS to $1..."
      systemctl restart replay.service
   else
      echo "Restart ReplayOS to $1: systemctl restart replay.service"
   fi
}

if $uninstall; then
   rm -rf "$OPT" "$CORES/240mp_libretro.so" "$CORE_CFG"
   remove_legacy
   remove_cma
   for s in /media/*; do
      rm -rf "$s/roms/$SYSTEM"
      $keep_settings || rm -rf "$s/240mp"
   done
   if $keep_settings; then
      echo "240-MP removed (settings kept in 240mp/ on the storage)"
   else
      echo "240-MP removed"
   fi
   restart_replay "drop the 240-MP system"
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
[ -x /opt/.240mp-new/240mp/bin/240mp ] && [ -f /opt/.240mp-new/240mp/core/240mp_libretro.so ] &&
   [ -f /opt/.240mp-new/240mp/core/240mp_libretro.cfg ] ||
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

# The registration (a regular file: ReplayOS rejects symlinks).
reg_new=$(cat "$OPT/core/240mp_libretro.cfg")
reg_changed=false
[ "$(cat "$CORE_CFG" 2>/dev/null)" = "$reg_new" ] || reg_changed=true
grep -q '^\[240mp\]' "$LEGACY_CFG" 2>/dev/null && reg_changed=true
printf '%s\n' "$reg_new" > "$CORE_CFG.240mp-tmp" && mv "$CORE_CFG.240mp-tmp" "$CORE_CFG"
remove_legacy

# The game entry goes on the storage ReplayOS uses now. 240-MP makes its
# settings folder there itself, bringing along settings from the SD card if
# there are any.
. "$OPT/bin/replayos-storage.sh"
storage=$(storage_dir)
mkdir -p "$storage/roms/$SYSTEM"
: > "$storage/roms/$SYSTEM/240-MP.lr"

remove_cma
echo "240-MP $(cat "$OPT/VERSION" 2>/dev/null || echo "") installed on $storage: open the 240-MP system"
echo "After changing the storage in ReplayOS, run this again."
$reg_changed && restart_replay "add the 240-MP system"
exit 0
