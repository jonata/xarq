#!/bin/bash
# Start the AppImage on a clean system: a bare container with only what any desktop has (graphics, fonts, X11),
# so that whatever the AppImage forgot to bundle shows up. Once on X11, once on Wayland.
#
#   docker run --rm -v "$PWD":/w -w /w ubuntu:24.04 bash .github/scripts/smoke-appimage.sh XarQ-*.AppImage
#
# Works in Debian/Ubuntu and Fedora images.
set -euo pipefail

appimage=$(realpath "$1")

# The runtime in front of the image must not need libfuse2.
offset=$(APPIMAGE_EXTRACT_AND_RUN=1 "$appimage" --appimage-offset)
if head -c "$offset" "$appimage" | grep -aq 'libfuse\.so\.2'; then
    echo "::error::the AppImage's runtime needs libfuse2"
    exit 1
fi

if command -v apt-get > /dev/null; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq xvfb xauth libegl1 libgl1 libfontconfig1 libdbus-1-3 libx11-xcb1 \
        libxkbcommon0 libglib2.0-0t64 weston > /dev/null
else
    dnf install -y -q xorg-x11-server-Xvfb xorg-x11-xauth mesa-libEGL mesa-libGL fontconfig \
        dbus-libs libX11-xcb libxkbcommon glib2 which weston > /dev/null
fi
export HOME=/tmp/smoke XDG_CONFIG_HOME=/tmp/smoke/config XDG_DATA_HOME=/tmp/smoke/data XDG_CACHE_HOME=/tmp/smoke/cache
mkdir -p "$HOME"

# No FUSE in a container: run it extracted.
cd /tmp && "$appimage" --appimage-extract > /dev/null

# The libraries it loads at runtime only show a problem when used: check here that they find what they need.
for path in /tmp/squashfs-root/usr/lib/lib{lcms2,raw,openjp2,jxl,djvulibre,jbig2dec,qrencode,archive,mobi}.so*; do
    [ -f "$path" ] || { echo "::error::$path is not in the AppImage"; exit 1; }
    if LD_LIBRARY_PATH=/tmp/squashfs-root/usr/lib ldd "$path" | grep 'not found'; then
        echo "::error::$path needs libraries a bare system lacks"
        exit 1
    fi
done

# run <log> <fail-regex> <command...>: passes if the command is still running after 20 seconds.
run() {
    local log=$1 fail=$2
    shift 2
    "$@" > "$log" 2>&1 &
    local pid=$!
    for _ in $(seq 20); do
        sleep 1
        if ! kill -0 "$pid" 2>/dev/null; then
            tail -40 "$log"
            echo "::error::$1 exited instead of running"
            exit 1
        fi
    done
    kill "$pid" 2>/dev/null || true
    sleep 2
    kill -9 "$pid" 2>/dev/null || true
    tail -15 "$log"
    if [ -n "$fail" ] && grep -Eq "$fail" "$log"; then
        grep -E "$fail" "$log" | sort -u | head -10
        echo "::error::it ran, but its output says it failed: /$fail/"
        exit 1
    fi
}

run /tmp/smoke-x11.log 'Could not load the Qt platform plugin|cannot open shared object' \
    xvfb-run -a /tmp/squashfs-root/AppRun

# On Wayland, with weston as a headless desktop. A window Qt cannot make there leaves the app running, so its
# output is checked as well. Not for the client buffer integration: XarQ's widgets draw in shared memory.
export XDG_RUNTIME_DIR=/tmp/smoke/run
mkdir -p -m 700 "$XDG_RUNTIME_DIR"
weston --backend=headless --socket=wayland-1 --idle-time=0 > /tmp/weston.log 2>&1 &
for _ in $(seq 40); do [ -S "$XDG_RUNTIME_DIR/wayland-1" ] && break; sleep 0.5; done
WAYLAND_DISPLAY=wayland-1 QT_QPA_PLATFORM=wayland run /tmp/smoke-wayland.log \
    'Could not create a shell surface|Failed to load shell integration|Could not load the Qt platform plugin|cannot open shared object' \
    /tmp/squashfs-root/AppRun
echo "started on X11 and Wayland"
