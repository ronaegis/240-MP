# Build environment for the ReplayOS core and 240-MP bundle: Debian trixie
# arm64, the same release (and glibc) ReplayOS is built on.
FROM debian:trixie
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config patchelf file \
    qt6-base-dev qt6-declarative-dev qt6-svg-dev qt6-svg-plugins qt6-qpa-plugins \
    qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtquick-effects qml6-module-qtqml-workerscript \
    libmpv-dev mpv libsdl2-dev libssl-dev libdrm-dev libgbm-dev libegl-dev libgles-dev libpcsclite-dev \
    >/dev/null && rm -rf /var/lib/apt/lists/*
