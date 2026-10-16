#!/usr/bin/env bash
# Install the ReplayOS core and the 240-MP bundle (from scripts/build-replayos.sh)
# on a ReplayOS box over SSH, and register the core.
#
# How ReplayOS picks a core: for each [section] of /opt/replay/cores/cores.cfg
# it matches "<section>/" against the content path, so anything under
# /media/sd/roms/240mp/ runs 240mp_libretro.so. An empty .lr file starts the
# core without content, which runs 240-MP.
#
#   scripts/deploy-replayos.sh [user@host]      (default root@replay)
set -euo pipefail
HOST="${1:-root@replay}"
B="$(cd "$(dirname "$0")/.." && pwd)/build-replayos"

scp -q "$B/240mp-replayos.tar.gz" "$HOST:/tmp/240mp-replayos.tar.gz"
scp -q "$B/240mp_libretro.so" "$HOST:/opt/replay/cores/.240mp_libretro.so.new"
scp -q "$B/240mp_test_child" "$HOST:/tmp/240mp_test_child"
ssh "$HOST" 'set -e
# Unpack beside the old install and swap directories: a running session keeps
# the files it has open. Same for the core: rename, never overwrite in place.
rm -rf /opt/.240mp-new && mkdir /opt/.240mp-new
tar -C /opt/.240mp-new -xzf /tmp/240mp-replayos.tar.gz && rm /tmp/240mp-replayos.tar.gz
mv /tmp/240mp_test_child /opt/.240mp-new/240mp/bin/
[ -d /opt/240mp ] && mv /opt/240mp /opt/.240mp-old
mv /opt/.240mp-new/240mp /opt/240mp && rm -rf /opt/.240mp-new /opt/.240mp-old
mv /opt/replay/cores/.240mp_libretro.so.new /opt/replay/cores/240mp_libretro.so

# Every bundled binary must resolve all its libraries on this system, through
# its own RPATH as at runtime (no LD_LIBRARY_PATH).
missing=$(find /opt/240mp/bin /opt/240mp/lib /opt/240mp/plugins /opt/240mp/qml -type f |
          while read -r f; do ldd "$f" 2>/dev/null | grep "not found" | sed "s|^|$f: |"; done)
if [ -n "$missing" ]; then echo "missing libraries:"; echo "$missing"; exit 1; fi

cfg=/opt/replay/cores/cores.cfg
[ -f "$cfg.orig" ] || cp "$cfg" "$cfg.orig"
grep -q "^\[240mp\]" "$cfg" || printf "\n[240mp]\nlow = \"240mp_libretro.so\"\nmid = \"240mp_libretro.so\"\nhi = \"240mp_libretro.so\"\n" >> "$cfg"
mkdir -p /media/sd/roms/240mp /media/sd/240mp
: > /media/sd/roms/240mp/240-MP.lr
printf "/roms/240mp/240-MP.lr" > "/media/sd/roms/_favorites/240mp@240-MP.lr.fav"
echo "installed: $(du -sh /opt/240mp | cut -f1) in /opt/240mp"'
