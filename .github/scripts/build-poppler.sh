#!/bin/bash
# Build Poppler into <dir>, for PDF import. pdfvector.cpp uses its private headers, which change between
# versions, so it is pinned to the version that code is written against: the systems' own are older (Ubuntu)
# or newer (MSYS2).
#
#   build-poppler.sh <dir>
set -euo pipefail

version=26.01.0
prefix=$1
command -v cygpath > /dev/null && prefix=$(cygpath -m "$prefix")
work=$(mktemp -d)
wget -q -O "$work/poppler.tar.xz" "https://poppler.freedesktop.org/poppler-$version.tar.xz"
tar -xf "$work/poppler.tar.xz" -C "$work"
cmake -S "$work/poppler-$version" -B "$work/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DENABLE_UNSTABLE_API_ABI_HEADERS=ON \
    -DENABLE_UTILS=OFF -DENABLE_CPP=OFF -DENABLE_GLIB=OFF -DENABLE_GOBJECT_INTROSPECTION=OFF \
    -DENABLE_QT5=OFF -DENABLE_QT6=OFF -DENABLE_GPGME=OFF -DENABLE_NSS3=OFF -DENABLE_BOOST=OFF -DENABLE_LIBCURL=OFF \
    -DBUILD_GTK_TESTS=OFF -DBUILD_QT5_TESTS=OFF -DBUILD_QT6_TESTS=OFF -DBUILD_CPP_TESTS=OFF -DBUILD_MANUAL_TESTS=OFF
cmake --build "$work/build" -j"$(nproc)"
cmake --install "$work/build"
rm -rf "$work"
