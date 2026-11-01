#!/bin/bash
# Run the self-test from the app's folder, so it finds what the package would: the app is windowed, but it
# writes to the pipe. The folder doesn't ship the offscreen plugin, so it is there only for this.
#
#   selftest-windows.sh <folder>
set -uo pipefail

dir=$1
cp /ucrt64/share/qt6/plugins/platforms/qoffscreen.dll "$dir/platforms/"
# The self-test writes its scratch files under /tmp, which to a Windows program is on the current drive.
mkdir -p "/$(pwd | cut -d/ -f2)/tmp"
fonts=$(dirname "$(find /ucrt64/share/fonts -name DejaVuSans.ttf | head -1)")
XARQ_SELFTEST_FONTS=$(cygpath -m "$fonts") QT_QPA_PLATFORM=offscreen QT_HASH_SEED=0 \
    "$dir/xarq.exe" --selftest > selftest.log 2>&1
code=$?
rm -f "$dir/platforms/qoffscreen.dll"
grep -aE 'FAIL|passed, ' selftest.log | tail -80
echo "self-test exit code: $code"
exit "$code"
