#!/bin/sh
# Source-checkout convenience wrapper; portable builds use pc/start_game.py.
set -eu
ROOT=$(CDPATH= cd "$(dirname "$0")" && pwd)
if [ -x "$ROOT/.venv/bin/python3" ]; then
    PYTHON="$ROOT/.venv/bin/python3"
elif command -v python3 >/dev/null 2>&1; then
    PYTHON=python3
else
    echo 'Python 3.10+ is required for first-run asset setup.' >&2
    exit 1
fi
SKIP_BUILD=0
FORCE_VCXSRV=0
for argument in "$@"; do
    case "$argument" in
        --prepare-only|--game|--game=*|--help|-h|--where) SKIP_BUILD=1 ;;
        --vcxsrv) FORCE_VCXSRV=1 ;;
    esac
done

echo 'Tokimeki Memorial: Forever with You — Native PC Port'
if [ "$SKIP_BUILD" -eq 0 ] && [ ! -f "$ROOT/pc/build/tokimeki-pc" ]; then
    echo 'Binary not found. Building with two workers...'
    if ! (cd "$ROOT" && make -f pc/Makefile -j2 all); then
        echo 'Build failed. It needs a C/C++ compiler (gcc, g++), make and the SDL2' >&2
        echo 'development package (Debian/Ubuntu: sudo apt install build-essential libsdl2-dev).' >&2
        exit 1
    fi
fi

# Keep the existing optional WSL/VcXsrv display selection.
if grep -qi microsoft /proc/version 2>/dev/null && command -v ip >/dev/null 2>&1; then
    WIN_IP=$(ip route | awk '/default/ {print $3; exit}')
    if [ "$FORCE_VCXSRV" -eq 1 ]; then
        if [ -z "$WIN_IP" ]; then
            echo 'Cannot determine the Windows host IP; set DISPLAY manually.' >&2
            exit 1
        fi
        echo "Forcing VcXsrv mode: DISPLAY=$WIN_IP:0"
        export DISPLAY="$WIN_IP:0"
    elif [ -n "$WIN_IP" ] && timeout 1 xdpyinfo -display "$WIN_IP:0" >/dev/null 2>&1; then
        echo "VcXsrv detected on $WIN_IP:0"
        export DISPLAY="$WIN_IP:0"
    fi
fi

# Preserve caller-relative disc paths: do not cd before this exec.
# No asset-folder symlinks, developer discs/, or fabricated screenshot crops.
TOKI_LAUNCHER=${TOKI_LAUNCHER:-./run_game.sh} exec "$PYTHON" "$ROOT/pc/start_game.py" --game "$ROOT/pc/build/tokimeki-pc" "$@"
