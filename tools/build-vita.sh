#!/usr/bin/env bash
# tools/build-vita.sh — TARGET=vita (dist/wacki.vpk), SDL2 (GXM) bez vitaGL.
#
# Toolchain, w tej kolejności:
#   1. lokalne VitaSDK (VITASDK ustawione albo /usr/local/vitasdk) z
#      oficjalnym SDL2 (GXM) — gdy nie ma Dockera albo przy WACKI_VITA_LOCAL=1;
#   2. obraz Dockera vitasdk/vitasdk.
# W obu przypadkach: jeśli libSDL2 ciągnie vitaGL (vglInitExtended), budowane
# jest oficjalne SDL2 (gałąź SDL2, tylko GXM). SDL2_mixer nie jest potrzebny.
# Dodatkowe argumenty idą do make.
set -euo pipefail
cd "$(dirname "$0")/.."

WACKI_VERSION=$(cat VERSION 2>/dev/null || git describe --tags --always --dirty 2>/dev/null || echo "dev")

# Wspólne dla obu dróg: czyste SDL2 + make. $1.. = argumenty make.
read -r -d '' INNER <<'SH' || true
set -e
export PATH=$VITASDK/bin:$PATH
if [ ! -f "$VITASDK/arm-vita-eabi/lib/libSDL2.a" ] ||
   nm -A "$VITASDK/arm-vita-eabi/lib/libSDL2.a" 2>/dev/null | grep -q vglInitExtended; then
    echo "==> buduję oficjalne SDL2 (GXM only)"
    rm -rf /tmp/SDL2-build /tmp/SDL2-src
    git clone --depth 1 --branch SDL2 https://github.com/libsdl-org/SDL.git /tmp/SDL2-src
    cmake -S /tmp/SDL2-src -B /tmp/SDL2-build \
        -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=$VITASDK/arm-vita-eabi \
        -DVIDEO_VITA_PIB=OFF -DVIDEO_VITA_PVR=OFF \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON
    cmake --build /tmp/SDL2-build -j"$(nproc)"
    cmake --install /tmp/SDL2-build
else
    echo "==> libSDL2 bez vitaGL — OK"
fi
make TARGET=vita WACKI_VERSION="$WACKI_VERSION" -j"$(nproc)" "$@"
SH

have_docker() { command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; }

if [ "${WACKI_VITA_LOCAL:-0}" = 1 ] || ! have_docker; then
    export VITASDK="${VITASDK:-/usr/local/vitasdk}"
    if [ ! -x "$VITASDK/bin/arm-vita-eabi-gcc" ]; then
        echo "error: brak Dockera i brak VitaSDK w $VITASDK" >&2
        exit 1
    fi
    echo "Lokalne VitaSDK: $VITASDK"
    WACKI_VERSION="$WACKI_VERSION" bash -c "$INNER" inner "$@"
else
    docker run --rm -v "$PWD:/wacki" -w /wacki \
        -e VITASDK=/usr/local/vitasdk -e WACKI_VERSION="$WACKI_VERSION" \
        vitasdk/vitasdk:latest sh -c "$INNER" inner "$@"
fi

echo "Gotowe: dist/wacki.vpk"
