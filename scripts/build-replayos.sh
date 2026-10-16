#!/usr/bin/env bash
# Build the ReplayOS core and a self-contained 240-MP bundle for it.
#
# Runs in Docker (Debian trixie arm64, packaging/replayos/Dockerfile) and
# produces build-replayos/240mp-replayos.tar.gz, 240-MP for /opt/240mp:
#   core/240mp_libretro.so    the core, which the installer copies to /opt/replay/cores
#   bin/                      240mp, the mpv stand-in, the launcher, yt-dlp and
#                             deno for the YouTube module, the test child
#   lib/                      (everything
#                             ReplayOS lacks, per packaging/replayos/system-libs.txt),
#                             plugins/, qml/, share/240mp/
# Install with scripts/deploy-replayos.sh (from this machine) or
# scripts/install-replayos.sh (on the Pi, from a release).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE=mp240-replayos-build
VERSION="${APP_VERSION:-dev}"

# yt-dlp and Deno (the JavaScript runtime current yt-dlp needs for YouTube),
# latest releases, verified against their published SHA-256 sums. yt-dlp is
# the standalone build: it does not depend on ReplayOS keeping its Python,
# and `yt-dlp -U` still updates it in place on the Pi.
DL="$ROOT/build-replayos/downloads"
mkdir -p "$DL"
fetch_verified() {  # <url> <file> <sums url> <name in sums>
   curl -fsSL "$3" | awk -v n="$4" -v f="$2" '$2 == n || $2 == "*" n { print $1 "  " f }' > "$DL/$2.sha256"
   [ -s "$DL/$2.sha256" ] || { echo "no checksum for $4" >&2; exit 1; }
   if ! (cd "$DL" && shasum -a 256 -c --status "$2.sha256" 2>/dev/null); then
      curl -fsSL -o "$DL/$2" "$1"
      (cd "$DL" && shasum -a 256 -c "$2.sha256")
   fi
}
GH=https://github.com
fetch_verified "$GH/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux_aarch64" yt-dlp \
   "$GH/yt-dlp/yt-dlp/releases/latest/download/SHA2-256SUMS" yt-dlp_linux_aarch64
fetch_verified "$GH/denoland/deno/releases/latest/download/deno-aarch64-unknown-linux-gnu.zip" deno.zip \
   "$GH/denoland/deno/releases/latest/download/deno-aarch64-unknown-linux-gnu.zip.sha256sum" deno-aarch64-unknown-linux-gnu.zip
rm -rf "$DL/deno" && unzip -q -o "$DL/deno.zip" -d "$DL/deno"

docker build --platform linux/arm64 -q -t "$IMAGE" "$ROOT/packaging/replayos" >/dev/null
docker run --rm --platform linux/arm64 -v "$ROOT:/src" -w /src -e VERSION="$VERSION" "$IMAGE" bash -euo pipefail -c '
B=build-replayos
cmake -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release -DMP240_LIBRETRO_BRIDGE=ON -DAPP_VERSION="$VERSION" \
      -DCMAKE_INSTALL_PREFIX=/opt/240mp . >/dev/null
cmake --build $B
rm -rf $B/stage && DESTDIR=$PWD/$B/stage cmake --install $B >/dev/null
OUT=$B/stage/opt/240mp
install -m 755 packaging/replayos/240mp-bridge $B/240mp_test_child $OUT/bin/
install -D -m 755 $B/240mp_libretro.so $OUT/core/240mp_libretro.so
echo "$VERSION" > $OUT/VERSION
install -m 755 $B/downloads/yt-dlp $OUT/bin/yt-dlp
install -m 755 $B/downloads/deno/deno $OUT/bin/deno

# Qt plugins and QML modules 240-MP uses (imports: QtQuick, QtQuick.Window,
# QtQuick.Effects; tls for HTTPS to media servers).
Q=/usr/lib/aarch64-linux-gnu/qt6
mkdir -p $OUT/plugins/platforms $OUT/plugins/egldeviceintegrations $OUT/qml/QtQuick
cp $Q/plugins/platforms/libqeglfs.so $OUT/plugins/platforms/
cp $Q/plugins/egldeviceintegrations/libqeglfs-kms-integration.so $OUT/plugins/egldeviceintegrations/
cp -r $Q/plugins/imageformats $Q/plugins/iconengines $Q/plugins/tls $OUT/plugins/
find $Q/qml/QtQuick -maxdepth 1 -type f -exec cp {} $OUT/qml/QtQuick/ \;
cp -r $Q/qml/QtQuick/Window $Q/qml/QtQuick/Effects $OUT/qml/QtQuick/
cp -r $Q/qml/QtQml $OUT/qml/

# Bundle every shared library ReplayOS does not have, until closed. The
# private helper of a library ReplayOS does have must come from the system as
# well: a bundled libpulsecommon next to the system libpulse is a version
# mismatch (missing symbols at startup).
mkdir -p $OUT/lib
grep -v "^#" packaging/replayos/system-libs.txt > /tmp/system-libs
for pass in 1 2 3 4; do
  find $OUT/bin/240mp $OUT/bin/mpv $OUT/lib $OUT/plugins $OUT/qml -name "*.so*" -o -name 240mp -type f -o -name mpv -type f | while read -r e; do
    ldd "$e" 2>/dev/null | awk "/=> \\//{print \$1\" \"\$3}" | while read -r name path; do
      case "$path" in */pulseaudio/*) continue;; esac
      grep -qxF "$name" /tmp/system-libs || [ -e "$OUT/lib/$name" ] || cp -L "$path" "$OUT/lib/$name"
    done
  done
done
# Every ELF finds lib/ relative to itself: a library with its own RUNPATH
# ignores the RPATH of the executable, and no LD_LIBRARY_PATH should leak into processes
# 240-MP starts. The executable gets DT_RPATH, which also covers dlopen().
find $OUT/lib $OUT/plugins $OUT/qml -type f -name "*.so*" | while read -r f; do
  patchelf --set-rpath "\$ORIGIN/$(realpath --relative-to="$(dirname "$f")" $OUT/lib)" "$f"
done
patchelf --force-rpath --set-rpath "\$ORIGIN/../lib" $OUT/bin/240mp $OUT/bin/mpv

tar -C $B/stage/opt -czf $B/240mp-replayos.tar.gz 240mp
echo "bundle: $(du -sh $OUT | cut -f1), $(ls $OUT/lib | wc -l) libraries"
'
ls -l "$ROOT/build-replayos/240mp-replayos.tar.gz"
