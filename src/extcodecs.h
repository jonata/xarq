// extcodecs.h — decoders that ride on runtime-optional shared libraries via dlopen (no dev headers,
// no link-time dependency). Same fail-safe contract as raw.cpp/icc.cpp: if the library is absent or
// any step fails, decode() returns a null QImage and the caller falls back. First-party policy:
// we LINK an existing library through its C ABI — we never shell out to a tool.
//   • JPEG 2000 (.jp2/.j2k/.j2c/.jpc/.jpx) via OpenJPEG (libopenjp2.so.7)
//   • JPEG XL   (.jxl)                     via libjxl (libjxl.so.0.x)
//   • Mobipocket/Kindle (.mobi/.azw/.azw3/.prc) cover image via libmobi (libmobi.so.0)
//   • DjVu (.djvu/.djv) first page via DjVuLibre (libdjvulibre.so.21)
#pragma once
#include <QImage>
#include <QString>
#include <QStringList>

namespace xara { namespace ExtCodec {

// Decode a file one of the optional libraries understands, or a null QImage.
QImage decode(const QString& path);
// Lower-case extensions we can decode when the backing library is loadable (for the Import filter).
QStringList extensions();
// Individual availability (for diagnostics / tests).
bool jp2Available();
bool jxlAvailable();
bool jxrAvailable();
bool dicomAvailable();
bool slideAvailable();
bool mobiAvailable();
bool djvuAvailable();
bool jbig2Available();

}} // namespace xara::ExtCodec
