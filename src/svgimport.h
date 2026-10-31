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
#include <QSizeF>
#include <QString>
#include <QVector>
#include <QRectF>
#include "model.h"

namespace xara {
// baseDir: directory to resolve relative <image> hrefs against (empty = only data-URI/absolute images).
// pagesOut (optional): receives the document-space rects of any Inkscape 1.2+ <inkscape:page> elements, so
// the caller can map a multi-page SVG onto real document pages.
// The VIEWPORT the document's content is laid out in: the root <svg>'s width/height when present
// (units resolved), else its viewBox extent. parseSvg maps viewBox -> viewport, so this — NOT the
// viewBox extent — is the size a page must take to match the imported artwork. librevenge writes
// the two in DIFFERENT units (width in inches, viewBox in points), so confusing them scales the
// page wrong by 33% or more. Null when the input has no usable root element.
QSizeF svgViewportSize(const QByteArray& xml);

QVector<Shape> parseSvg(const QByteArray& xml, QString* error = nullptr, const QString& baseDir = QString(),
                        QVector<QRectF>* pagesOut = nullptr);
// Parse an SVG/ODF path-data string ("M..C..Z") into a QPainterPath (shared with the ODF importer).
QPainterPath parseSvgPathData(const QString& d);
}
