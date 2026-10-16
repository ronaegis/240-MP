#!/usr/bin/env bash
# Build the ReplayOS core and a self-contained 240-MP bundle for it.
#
# Runs in Docker (Debian trixie arm64, packaging/replayos/Dockerfile) and
# produces, in build-replayos/:
#   240mp_libretro.so         the core, for /opt/replay/cores
#   240mp-replayos.tar.gz     240-MP for /opt/240mp: bin/ (240mp, the mpv
#                             stand-in, the launcher), lib/ (everything
#                             ReplayOS lacks, per packaging/replayos/system-libs.txt),
#                             plugins/, qml/, share/240mp/
# Install with scripts/deploy-replayos.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE=mp240-replayos-build
VERSION="${APP_VERSION:-dev}"

docker build --platform linux/arm64 -q -t "$IMAGE" "$ROOT/packaging/replayos" >/dev/null
docker run --rm --platform linux/arm64 -v "$ROOT:/src" -w /src -e VERSION="$VERSION" "$IMAGE" bash -euo pipefail -c '
B=build-replayos
cmake -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release -DMP240_LIBRETRO_BRIDGE=ON -DAPP_VERSION="$VERSION" \
      -DCMAKE_INSTALL_PREFIX=/opt/240mp . >/dev/null
cmake --build $B
rm -rf $B/stage && DESTDIR=$PWD/$B/stage cmake --install $B >/dev/null
OUT=$B/stage/opt/240mp
install -m 755 packaging/replayos/240mp-bridge $OUT/bin/

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
ls -l "$ROOT/build-replayos/240mp_libretro.so" "$ROOT/build-replayos/240mp-replayos.tar.gz"
