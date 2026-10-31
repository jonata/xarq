// render.h — paint the document model with QPainter. Shared by the live canvas,
// layer thumbnails and PNG/SVG export so they always look identical.
#pragma once
#include "model.h"
#include <QSet>
class QPainter;

namespace xara {
// Paint one shape (fill + gradient, stroke, drop-shadow, contour, object opacity
// and blend mode) into the painter, which must already be in DOCUMENT coordinates.
void paintShape(QPainter& g, const Shape& s);
// Paint the whole document (page already laid out by the caller).
void paintDocument(QPainter& g, const Document& d);
void setOutlineRenderMode(bool on);   // wireframe view mode (screen only; export never sets it)
void setScreenRenderMode(bool on);    // bracket the on-screen paint so non-printing layers still show on canvas
void setRenderCullRect(const QRectF& docRect);   // skip shapes wholly outside this doc-space rect (canvas only; invalid ⇒ disabled)
void setRenderViewport(const QRectF& devRect);   // visible viewport rect in the painter's DEVICE space (the widget in
                                                 // logical px); effect buffers (blur/feather/bevel/…) are clipped to it so
                                                 // their per-frame cost is bounded by the screen, not the shape. Canvas only;
                                                 // invalid ⇒ effects render at full extent (export path — byte-identical).
// Drag-proxy support (canvas only). The canvas splits the document into a static bitmap (rest of doc)
// and a moving bitmap (the dragged selection) so a live move/resize/rotate never re-rasterises the whole
// document. These select WHICH top-level shapes paintDocument draws, keyed by the Shape's address:
//   • skip-set : draw everything EXCEPT these (builds the static backdrop minus the movers).
//   • only-set : draw ONLY these               (builds the moving-selection bitmap).
// Both empty ⇒ no effect ⇒ byte-identical to the normal render, so export/print/corpus never filter.
void setRenderSkipSet(const QSet<const void*>& shapes);
void setRenderOnlySet(const QSet<const void*>& shapes);   // empty ⇒ inactive (draw all); non-empty ⇒ draw only these
// Conservative upper bound (doc px) on how far a shape's pixels extend beyond its geometry bounds via
// effects (stroke ½, shadow, contour, extrude, feather, blur). Used as a safe bitmap-proxy capture margin.
double shapeRenderReach(const Shape& s);
// Build a QBrush for a fill in the shape's local space mapped by xf (local->device).
QBrush fillBrush(const Fill& f, const QRectF& localBounds);
} // namespace xara
