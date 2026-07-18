// render.h — paint the document model with QPainter. Shared by the live canvas,
// layer thumbnails and PNG/SVG export so they always look identical.
#pragma once
#include "model.h"
class QPainter;

namespace xara {
// Paint one shape (fill + gradient, stroke, drop-shadow, contour, object opacity
// and blend mode) into the painter, which must already be in DOCUMENT coordinates.
void paintShape(QPainter& g, const Shape& s);
// Paint the whole document (page already laid out by the caller).
void paintDocument(QPainter& g, const Document& d);
void setOutlineRenderMode(bool on);   // wireframe view mode (screen only; export never sets it)
void setScreenRenderMode(bool on);    // bracket the on-screen paint so non-printing layers still show on canvas
// Build a QBrush for a fill in the shape's local space mapped by xf (local->device).
QBrush fillBrush(const Fill& f, const QRectF& localBounds);
} // namespace xara
