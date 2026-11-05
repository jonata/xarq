#!/bin/bash
# Builds an installable XarQ package on Haiku from an existing build directory:
#     src/haiku/make-hpkg.sh [build-dir] [output-dir]
# Install the result by copying it to ~/config/packages, or double-click it in Tracker.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${1:-$HERE/../build}
OUT=${2:-$PWD}
VERSION=26.10~alpha
REVISION=1
ARCH=$(getarch)
STAGE=$(mktemp -d /tmp/xarq-hpkg.XXXXXX)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/apps" "$STAGE/data/deskbar/menu/Applications"
# copyattr keeps the attributes mimeset wrote; the icon and signature are resources inside the binary itself.
copyattr -d "$BUILD/xarq" "$STAGE/apps/XarQ"
strip --strip-unneeded "$STAGE/apps/XarQ"
ln -s ../../../../apps/XarQ "$STAGE/data/deskbar/menu/Applications/XarQ"

# The file type for Xara documents, and the Ubuntu font licence Haiku doesn't ship.
mkdir -p "$STAGE/data/mime_db/application" "$STAGE/data/licenses"
rc -o "$STAGE/xar-type.rsrc" "$HERE/x-vnd.xara-xar.rdef"
resattr -O -o "$STAGE/data/mime_db/application/x-vnd.xara-xar" "$STAGE/xar-type.rsrc"
rm "$STAGE/xar-type.rsrc"
cp "$HERE/licenses/Ubuntu Font License 1.0" "$STAGE/data/licenses/"

# Everything the binary links comes straight from its dynamic section, so this list cannot drift from the build.
# The base system provides the ones filtered out. Package names use underscores where a library's name has dashes.
LINKED=$(objdump -p "$STAGE/apps/XarQ" | awk '/NEEDED/ { print $2 }' | sed -e 's/\.so.*$//' \
    -e '/^libroot$/d' -e '/^libbe$/d' -e '/^libnetwork$/d' -e '/^libstdc++$/d' -e '/^libgcc_s$/d' \
    -e 's/-/_/g' | sort -u)

{
    cat <<EOF
name			xarq
version			$VERSION-$REVISION
architecture	$ARCH
summary			"Vector illustration and page layout, descended from Xara"
description		"XarQ is a vector illustration and page layout application with the complete classic Xara tool set. It reads and writes native XAR documents and imports a wide range of vector and raster formats."
packager		"Jonatã Bolzan Loss <jonata@jonata.org>"
vendor			"XarQ"
copyrights		{
	"1993-2006 Xara Group Ltd"
	"2026 Jonatã Bolzan Loss"
}
licenses		{
	"GNU GPL v2"
	"SIL Open Font License v1.1"
	"Ubuntu Font License 1.0"
}
provides		{
	xarq = $VERSION
	app:XarQ = $VERSION
}
requires		{
	haiku
	qt6_imageformats
	lib:liblcms2
	lib:libraw
	lib:libopenjp2
	lib:libqrencode
	lib:libdjvulibre
	lib:libjbig2dec
	dejavu
	hyphen
	myspell
EOF
    for lib in $LINKED; do printf '\tlib:%s\n' "$lib"; done
    cat <<EOF
}
urls			{
	"https://github.com/jonata/xarq"
}
EOF
} > "$STAGE/.PackageInfo"

PACKAGE="$OUT/xarq-$VERSION-$REVISION-$ARCH.hpkg"
rm -f "$PACKAGE"
ABS_PACKAGE=$(cd "$OUT" && pwd)/$(basename "$PACKAGE")
(cd "$STAGE" && package create -q "$ABS_PACKAGE")
package list "$ABS_PACKAGE" | head -12
echo "Created $ABS_PACKAGE"
