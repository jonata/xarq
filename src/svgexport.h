// svgexport.h — serialize the document model back to an SVG 1.1 document.
// The inverse of svgimport: every Shape becomes an SVG element (rect/ellipse/path/
// text/g/image), fills/strokes/opacity/transforms are mapped to attributes, and
// gradients are emitted into <defs>. Round-trips through parseSvg for vector content.
#pragma once
#include <QByteArray>
#include <QVector>
#include <QRectF>
#include "model.h"

namespace xara {
// Serialize shapes (in document space) to SVG; `viewBox` sets the SVG viewBox/size
// (typically the content bounds or page rect).
QByteArray writeSvg(const QVector<Shape>& shapes, QRectF viewBox);
}
