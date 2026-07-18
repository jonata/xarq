// pathedit.h — bezier-aware decomposition of a QPainterPath into editable nodes.
// A QPainterPath stores cubics as (CurveTo c1)(CurveToData c2)(CurveToData end);
// for editing we want the natural representation: a list of on-curve anchors, each
// carrying an optional incoming and outgoing control handle. These helpers convert
// both ways losslessly so the Node tool can drag handles without flattening curves.
#pragma once
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QVector>

namespace xara {

struct PathNode {
    QPointF anchor;
    QPointF in, out;              // control handles (absolute coords)
    bool    hasIn=false, hasOut=false;
    bool    smooth=false;         // handles kept collinear when dragged (tangent continuity)
    bool    symmetric=false;      // stronger: handles also kept EQUAL length (point-mirror). Implies smooth.
};

struct SubPath {
    QVector<PathNode> nodes;
    bool closed=false;
};

QVector<SubPath> pathToNodes(const QPainterPath& path);
QPainterPath     nodesToPath(const QVector<SubPath>& subs);

// ---- Envelope / mould warp: a Coons patch over a 4-sided cubic-bezier cage ----
// The cage edges run cor0->cor1->cor2->cor3->cor0; cor[i] starts edge i and ec1[i]/ec2[i] are
// its two control points. UV mapping: (0,0)=bottom-left, (0,1)=top-left, (1,1)=top-right,
// (1,0)=bottom-right. Shared by the XAR reader (reads Xara envelopes) and the interactive
// Mould tool (authors them), so both stay bit-for-bit consistent.
struct Envelope { QPointF cor[4], ec1[4], ec2[4]; };
// Map a normalised (u,v) in [0,1]² to its warped doc-space point on the Coons surface.
QPointF      coonsWarp(const Envelope& e, double u, double v);
// Flatten `docPath` to polylines, densify long segments, and warp every vertex through the cage,
// treating `src` as the undistorted rectangle whose corners map to the cage corners.
QPainterPath warpThroughEnvelope(const QPainterPath& docPath, const QRectF& src, const Envelope& e);
// True when any edge's control points bow more than `tol` off the straight chord (=> a curved warp
// that must be baked into geometry, rather than a plain quad that a projective transform can carry).
bool         envelopeIsCurved(const Envelope& e, double tol=1.5);

} // namespace xara
