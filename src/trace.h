// trace.h — Trace Bitmap: convert a raster image into editable vector Shapes.
// Posterizes to N colours (median cut), extracts each colour region's outline
// (holes included), simplifies (Ramer–Douglas–Peucker) and optionally smooths
// to bezier curves, and returns filled Path shapes in the image's pixel space
// (back-to-front by area). Toolkit-neutral: the caller places/scales the result.
#pragma once
#include <QImage>
#include <QVector>
#include "model.h"

namespace xara {

struct TraceOptions {
    int  colors    = 6;     // palette size (2..64)
    bool smooth    = true;  // Catmull-Rom bezier smoothing vs polylines
    int  minAreaPx = 24;    // drop regions smaller than this (noise)
    int  maxDim    = 240;   // downscale so the longest side ≤ this before tracing
    double epsilon = 1.2;   // RDP tolerance (downscaled px)
};

// Returns Path shapes whose local geometry is in ORIGINAL-image pixel coordinates
// (the downscale is undone via each shape's transform).
QVector<Shape> traceBitmap(const QImage& src, const TraceOptions& opt = {});

// Extract a representative colour palette from an image (median cut), most-dominant first.
QVector<QColor> extractPalette(const QImage& src, int colors = 6);

}
