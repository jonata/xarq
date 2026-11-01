#!/bin/bash
# Gather the app's folder for Windows, in an MSYS2 UCRT64 shell: the executable, Qt's plugins, every DLL they
# need, and the optional DLLs XarQ loads at runtime when present.
#
#   bundle-windows.sh <xarq.exe> <folder>
set -euo pipefail

exe=$1
dir=$2
bin=/ucrt64/bin

mkdir -p "$dir"
cp "$exe" "$dir/"
deploy=$(command -v windeployqt6 || command -v windeployqt-qt6 || echo /ucrt64/share/qt6/bin/windeployqt)
"$deploy" --no-translations --no-opengl-sw --no-system-d3d-compiler --no-compiler-runtime "$dir/$(basename "$exe")"

# Nothing links to these: dlcompat.cpp looks for them by name in the app's folder.
for pattern in 'liblcms2-*.dll' 'libraw-*.dll' 'libraw_r-*.dll' 'libopenjp2-*.dll' 'libjxl.dll' 'libjxl-*.dll' \
               'libdjvulibre-*.dll' 'libjbig2dec-*.dll' 'libjbig2dec.dll' 'libqrencode*.dll' 'libarchive-*.dll' \
               'libmobi-*.dll' 'libmobi.dll'; do
    for dll in "$bin"/$pattern; do
        [ -f "$dll" ] && cp -n "$dll" "$dir/" && echo "runtime: $(basename "$dll")"
    done
done

# Every DLL of MSYS2's that anything in the folder needs, until nothing new turns up. ldd lists the whole tree
# of each file, and Windows' own DLLs under other paths.
while :; do
    added=0
    while IFS= read -r dll; do
        name=$(basename "$dll")
        if [ ! -e "$dir/$name" ]; then
            cp "$dll" "$dir/"
            added=1
        fi
    done < <(find "$dir" \( -name '*.exe' -o -name '*.dll' \) -print0 | xargs -0 -n 16 ldd \
        | awk -v bin="$bin" 'index($3, bin) == 1 { print $3 }' | sort -u)
    [ "$added" = 0 ] && break
done

find "$dir" \( -name '*.exe' -o -name '*.dll' \) -print0 | xargs -0 strip --strip-unneeded 2>/dev/null || true
du -sh "$dir"
find "$dir" -maxdepth 1 -name '*.dll' | wc -l
