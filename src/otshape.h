// otshape.h — real OpenType feature shaping (Batch R9). Shapes a line of text through HarfBuzz with a
// set of feature tags (liga/smcp/onum/tnum/frac/kern/…) and returns the glyph outlines via
// QRawFont::pathForGlyphIndex. HarfBuzz and QRawFont share the SAME font (bridged through
// QRawFont::fontTable), so glyph indices line up. Default (empty features) ≈ Qt's own layout.
#pragma once
#include <QString>
#include <QStringList>
#include <QFont>
#include <QPainterPath>

namespace xara {

bool otAvailable();   // HarfBuzz shaping usable for `font`

// Shape one line (no '\n'): returns the outline path, baseline at y=0, pen starting at x=0.
// `features` are HarfBuzz feature strings ("smcp", "onum", "+liga", "-liga", "frac", …).
// `tracking` is extra letter-spacing in px added after each glyph (QRawFont drops QFont letter-spacing).
// `advanceOut`/`glyphCountOut` receive the total pen advance (px) and the shaped glyph count.
// `hadMissingOut` is set true if any character mapped to .notdef (glyph 0) — the caller should then
// fall back to Qt's own layout (which does font substitution) rather than render tofu.
QPainterPath otShapeLine(const QString& text, const QFont& font, const QStringList& features,
                         double tracking = 0.0, double* advanceOut = nullptr,
                         int* glyphCountOut = nullptr, bool* hadMissingOut = nullptr);

} // namespace xara
