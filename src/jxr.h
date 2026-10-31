// jxr.h — JPEG XR / HD Photo (.jxr / .wdp / .hdp / .wmp) decode via Microsoft's reference jxrlib
// (libjxrglue + libjpegxr, the same code the format was standardised from). Linked, not dlopen'd,
// because the jxrglue decode API is a deep function-pointer vtable that would be error-prone to
// self-declare. Built only when libjxr is found at configure time (HAVE_JXR); otherwise jxrDecode
// returns a null QImage and jxrAvailable() is false, and the importer falls back like any other codec.
#pragma once
#include <QImage>
#include <QString>

namespace xara {
// Decode the first frame of a JPEG XR file to an 8-bit QImage (ARGB32 when the image carries alpha,
// RGB32 otherwise), or a null QImage on any failure. Bounded: rejects absurd dimensions.
QImage jxrDecode(const QString& path);
// True only when compiled against libjxr.
bool   jxrAvailable();
}
