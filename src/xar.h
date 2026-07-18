// xar.h — native Xara XAR file format reader/writer.
//
// XAR is the native binary format of Xara X / Xara Xtreme (and this project's engine).
// It is a little-endian stream of records — each `tag(u32) size(u32) data[size]` —
// preceded by an 8-byte magic, organised into a tree by UP/DOWN navigation records
// (Document > Chapter > Spread > Layer > objects). Coordinates are millipoints
// (1/1000 pt, 72000 dpi) with the origin at the page's bottom-left and y increasing
// upward. See the Xar Format Specification.
//
// This implements a faithful, interoperable subset: the document framework, paths
// (the universal geometry record), flat / linear / radial fills with a real colour
// table, line colour + width, and groups. Files written here open in Xara; real Xara
// files (including zlib-compressed ones) load here on a best-effort basis.
#pragma once
#include <QString>
#include "model.h"

namespace xara {

// Write `doc` to `path` as an (uncompressed, fully valid) XAR file.
bool saveXar(const Document& doc, const QString& path, bool compress=false);   // compress=true deflates the record body (Xara STARTCOMPRESSION)

// Load a XAR file (compressed or not) into `doc`. Returns false on a bad/again unreadable file.
bool loadXar(Document& doc, const QString& path);

// Generate a plasma/fractal cloud tile ramping between two colours (Xara's PlasmaFractalFill).
// Exposes the XAR reader's generator so the UI can apply fractal fills to new objects.
QImage makeFractalFill(const QColor& c0, const QColor& c1, int dim = 128, int seed = 12345);

} // namespace xara
