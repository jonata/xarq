// rasterextra.h — first-party native decoders for raster formats Qt's QImage can't read.
// Policy: no external tools, no heavyweight deps — each decoder is a small, self-contained,
// bounds-checked reader built from the format spec. Formats: QOI, PCX, SGI/RGB, Radiance HDR
// (RGBE), Portable FloatMap (PFM), Netpbm PAM (P7), DirectDraw Surface (DDS: DXT1/3/5 + raw).
#pragma once
#include <QImage>
#include <QString>
#include <QStringList>
#include <QByteArray>

namespace xara { namespace RasterExtra {

// Decode a file this module understands, or return a null QImage. Sniffs by magic bytes first,
// falling back to the extension when a format has no reliable signature (PFM/PAM share tokens).
QImage decode(const QString& path);
QImage decodeData(const QByteArray& data, const QString& extHint = QString());

// Lower-case extensions (no dot) this module can decode natively — for the Import dialog filter.
const QStringList& extensions();
// Extensions Qt already decodes via installed image plugins but that we want surfaced in the filter.
const QStringList& qtExtraExtensions();
// True when the path's magic/extension is one of ours (cheap check; does not fully decode).
bool canDecode(const QString& path);

}} // namespace xara::RasterExtra
