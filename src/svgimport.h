// svgimport.h — parse an SVG document into editable Shapes.
// Supports rect/circle/ellipse/line/polyline/polygon/path/g, a full path-data
// parser (M/L/H/V/C/S/Q/T/A/Z abs+rel), transforms (translate/scale/rotate/matrix/
// skew), fill/stroke/opacity from attributes/inline style/author CSS classes,
// linear+radial gradients (url(#id)), <text>/<tspan>, and <use>/<symbol>/<defs>
// referencing (viewBox→viewport, forward refs, nested use, cycle-guarded), and
// <image> rasters (data-URI, or a LOCAL file resolved against baseDir), and CSS
// length units (em/pt/pc/mm/cm/in → px). <use> of <text> and per-glyph text
// positioning are still skipped. Toolkit-neutral: returns model Shapes, so the
// caller places them in the document.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include "model.h"

namespace xara {
// baseDir: directory to resolve relative <image> hrefs against (empty = only data-URI/absolute images).
QVector<Shape> parseSvg(const QByteArray& xml, QString* error = nullptr, const QString& baseDir = QString());
}
