#!/bin/bash
# Package src/build/xarq as XarQ-<version>-x86_64.AppImage.
#
#   build-appimage.sh <version>
#
# linuxdeploy gathers the libraries and Qt's plugins into the AppDir. The AppImage itself comes from a pinned
# appimagetool and a static runtime, so it opens without libfuse2, which Ubuntu no longer installs.
set -euo pipefail

version=$1
tools=$(mktemp -d)
export APPIMAGE_EXTRACT_AND_RUN=1

base=https://github.com/linuxdeploy
wget -q -O "$tools/linuxdeploy" "$base/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
wget -q -O "$tools/linuxdeploy-plugin-qt" "$base/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
wget -q -O "$tools/appimagetool" https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
wget -q -O "$tools/runtime" https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
sha256sum -c - <<EOF
ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0  $tools/appimagetool
2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d  $tools/runtime
EOF
chmod +x "$tools"/*
export PATH="$tools:$PATH"

# The libraries XarQ loads at runtime: nothing links to them, so linuxdeploy has to be told. libOpenGL is on
# linuxdeploy's exclude list, but Qt needs it and not every desktop has it; it only dispatches to the system's GL.
libs=()
for lib in liblcms2.so.2 libraw.so.23 libopenjp2.so.7 libjxl.so.0.7 libdjvulibre.so.21 libjbig2dec.so.0 \
           libqrencode.so.4 libarchive.so.13 libmobi.so.0 libOpenGL.so.0; do
    path=$(ldconfig -p | awk -v l="$lib" '$1 == l && /x86-64/ { print $NF; exit }')
    if [ -z "$path" ]; then
        echo "::error::$lib is not installed"
        exit 1
    fi
    libs+=(--library "$path")
done

cp src/assets/logo/xarq-appicon.svg "$tools/xarq.svg"
export QMAKE=/usr/bin/qmake6
export EXTRA_QT_PLUGINS="wayland-shell-integration;wayland-decoration-client;wayland-graphics-integration-client"
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so"
linuxdeploy --appdir AppDir --executable src/build/xarq \
    --desktop-file src/assets/xarq.desktop --icon-file "$tools/xarq.svg" \
    "${libs[@]}" --plugin qt

ARCH=x86_64 appimagetool --runtime-file "$tools/runtime" AppDir "XarQ-$version-x86_64.AppImage"
ls -l "XarQ-$version-x86_64.AppImage"
du -sh AppDir
rm -rf "$tools"
