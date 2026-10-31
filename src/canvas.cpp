#include "canvas.h"
#include "pdfvector.h"
#include "render.h"
#include "pathedit.h"
#include "photoops.h"
#include "svgimport.h"
#include "svgexport.h"
#include "trace.h"
#include "dialogs.h"
#include "exif.h"
#include "spell.h"
#include "qr.h"
#include "prepress.h"
#include "icc.h"
#include "raw.h"
#include "rasterextra.h"
#include "vecextra.h"
#include "gisvector.h"
#include "dwgvector.h"
#include "affvector.h"
#include "extcodecs.h"
#include "xar.h"
#include <QPainter>
#include <QSettings>
#include <QElapsedTimer>
#include "cdrvector.h"
#include "extvector.h"
#include "wmf.h"
#include <QTimer>
#include <QApplication>
#include <QCursor>
#include "icons.h"     // toolIcon() — the Ctrl radial menu's inner ring draws tool icons
#include <QFontInfo>
#include <QSet>
#include <QClipboard>
#include <QUrl>
#include <QFileInfo>
#include <QDir>
#include <QDate>
#include <QMimeData>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QMovie>
#include <QXmlStreamReader>
#include <QBuffer>
#include <QTextStream>
#include <QInputDialog>
#include <QRegularExpression>
#include <QtMath>
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QProcess>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextFragment>
#include <algorithm>
#include <queue>
#include <vector>
#include <cmath>

using namespace xara;

// Above this many QPainterPath elements, the selection silhouette is not stroked every frame during a drag
// (bbox + handles + the moving-proxy bitmap suffice); it returns in full when the drag ends. See drawSelection.
static const int kDragOutlineStrokeCap = 1500;
// Max device-pixel area for the drag-proxy capture. Beyond this the moving bitmap is rendered at reduced
// resolution (and drawn back up to size) so the ONE-TIME build can't freeze the UI on a heavy group / HiDPI
// screen. ~6 Mpx: bigger than a typical object at dpr 1, so ordinary drags capture at full resolution.
static const qint64 kDragProxyBudgetPx = 6LL*1024*1024;
// Above this many objects in the document, per-object snapping is skipped during a move (page edges, guides and
// the grid still snap). Scanning every object's bounds each frame — with an O(M) membership test per object —
// is O(N·M) and freezes a heavy doc; and past a few thousand candidates the 6 px snap threshold always finds
// *something*, so object snapping is meaningless noise anyway. See snapAdjust.
static const int kSnapObjectCandidateCap = 4000;
static const QColor kBg(43, 45, 51);          // workspace
static const QColor kBgDark(33, 35, 40);
static const QColor kAccent(70, 150, 245);
static const QColor kHandle(255, 255, 255);

Canvas::Canvas(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::ArrowCursor);
    // Every committed document change emits documentChanged → invalidate the screen-render cache.
    connect(this, &Canvas::documentChanged, this, [this]{ m_cacheDirty = true; });
    // The static selection-outline cache is invalid whenever the selection or the document changes.
    connect(this, &Canvas::documentChanged,  this, [this]{ m_selCacheValid = false; });
    connect(this, &Canvas::selectionChanged, this, [this]{ m_selCacheValid = false; });
    // Coalesce documentChanged into documentChangedDebounced. One-off edits (menu ops, typing, programmatic
    // changes — anything with no live drag in flight) forward immediately so panels stay in lock-step and
    // the self-test sees synchronous refreshes. While a drag is emitting documentChanged every frame, the
    // heavy O(N) galleries/panels instead get a single trailing refresh ~45 ms after the gesture settles.
    m_docDebounceTimer = new QTimer(this);
    m_docDebounceTimer->setSingleShot(true);
    m_docDebounceTimer->setInterval(45);
    connect(m_docDebounceTimer, &QTimer::timeout, this, [this]{ emit documentChangedDebounced(); });
    connect(this, &Canvas::documentChanged, this, [this]{
        if (manipulatingContent()) m_docDebounceTimer->start();
        else { m_docDebounceTimer->stop(); emit documentChangedDebounced(); }
    });
    setAcceptDrops(true);   // colour swatches can be dragged from the Colour Line onto objects
    m_fill.type = FillType::Solid; m_fill.color = QColor(74,144,226);
    m_stroke.enabled = true; m_stroke.color = QColor(28,32,40); m_stroke.width = 1.5;
    { QColor gh(QSettings("XarQ","XarQ").value("gradHandleColor").toString()); if(gh.isValid()) m_gradHandleColor=gh; }
    pushUndo(QStringLiteral("init"));
    m_undo.clear(); m_undoLabels.clear();
}

// ---- view transforms -------------------------------------------------------
QTransform Canvas::viewXf() const {
    QTransform t;
    t.translate(m_origin.x(), m_origin.y());
    t.scale(m_zoom, m_zoom);
    return t;
}
QPointF Canvas::toDoc(const QPointF& w) const { return viewXf().inverted().map(w); }
QPointF Canvas::toView(const QPointF& d) const { return viewXf().map(d); }

// Ctrl-constrain while drawing a NEW shape (Xara's core drawing affordance):
//  • Rect→square / Ellipse→circle / Polygon→regular: square the drag box (equal |dx|,|dy|, keeping the
//    drag direction so it still grows toward the cursor).
QPointF Canvas::constrainSquareCorner(const QPointF& a, const QPointF& b){
    const double dx=b.x()-a.x(), dy=b.y()-a.y(); const double s=qMax(std::abs(dx),std::abs(dy));
    return QPointF(a.x()+(dx<0?-s:s), a.y()+(dy<0?-s:s));
}
//  • Line→angle snap: snap the endpoint direction to the nearest 15° (covers 0/45/90°), keeping length.
QPointF Canvas::constrainLineAngle(const QPointF& a, const QPointF& b){
    const double dx=b.x()-a.x(), dy=b.y()-a.y(); const double len=std::hypot(dx,dy);
    if(len<1e-6) return b;
    const double step=M_PI/12.0; const double ang=std::round(std::atan2(dy,dx)/step)*step;
    return QPointF(a.x()+std::cos(ang)*len, a.y()+std::sin(ang)*len);
}

void Canvas::setDocument(const Document& d, const QString& path) {
    m_cropMode=false; m_cropShape=-1;   // any in-progress crop is invalid against a new document
    m_straightenMode=false; m_straightenShape=-1; m_straightenGrab=-1;   // ...as is an in-progress straighten (stale shape index)
    m_doc = d; m_path = path; m_sel.clear(); resetGroupEntry(); resetCloneState(); resetMaskStroke(); invalidateMould(); resetTextEdit();   // clone/mask/mould/text-edit state is document-specific
    m_guideHpos.clear(); m_guideVpos.clear();   // ruler guides are per-document — a new doc starts with none
    m_undo.clear(); m_redo.clear(); m_undoLabels.clear(); m_redoLabels.clear(); pushUndo("open"); m_undo.clear(); m_undoLabels.clear();
    emit selectionChanged(); emit documentChanged(); update();
}
void Canvas::newDocument() { setDocument(Document{}, QString()); zoomFit(); }

Canvas::Session Canvas::captureSession() const {
    Session s;
    s.doc=m_doc; s.path=m_path; s.sel=m_sel; s.undo=m_undo; s.redo=m_redo;
    s.activeLayer=m_activeLayer; s.zoom=m_zoom; s.origin=m_origin;
    s.guidesH=m_guideHpos; s.guidesV=m_guideVpos;
    return s;
}
void Canvas::loadSession(const Session& s) {
    // Abort any in-progress interaction that referenced the outgoing document.
    m_cropMode=false; m_cropShape=-1; m_straightenMode=false; m_straightenShape=-1; m_straightenGrab=-1; resetCloneState(); resetMaskStroke();
    m_penActive=false; m_penHandleDrag=false; m_penResume=-1; m_penResumeBase=QPainterPath(); m_nodeShape=-1; m_nodeChild=-1; m_mouldShape=-1;
    m_dragging=false; m_panning=false; m_rotating=false; m_activeHandle=HNone; m_rubber=false;
    m_doc=s.doc; m_path=s.path; m_sel=s.sel; m_undo=s.undo; m_redo=s.redo;
    // Undo labels aren't stashed in a Session — keep the parallel lists sized so undoLabel()/redoLabel() stay valid.
    m_undoLabels=QStringList(); for(int i=0;i<m_undo.size();++i) m_undoLabels<<QString();
    m_redoLabels=QStringList(); for(int i=0;i<m_redo.size();++i) m_redoLabels<<QString();
    m_guideHpos=s.guidesH; m_guideVpos=s.guidesV;
    m_activeLayer=qBound(0, s.activeLayer, qMax(0, m_doc.layers.size()-1));
    m_zoom=s.zoom>0 ? s.zoom : 1.0; m_origin=s.origin;
    // Drop selection indices that no longer exist in this document.
    for(int i=m_sel.size()-1;i>=0;--i) if(m_sel[i]<0 || m_sel[i]>=m_doc.shapes.size()) m_sel.remove(i);
    emit selectionChanged(); emit documentChanged(); update();
}

void Canvas::setTool(Tool t) {
    if (m_tool == t) return;
    if (m_textEdit>=0) endTextEdit();   // leaving the current tool commits any in-canvas text edit
    m_tool = t; m_penActive=false; m_penAnchors.clear(); m_penHandles.clear(); m_freePts.clear();
    if (t!=Tool::Select) resetGroupEntry();   // leaving the Select tool drops any group descent (the group stays selected at top level)
    m_nodeShape=-1; m_nodeChild=-1; m_nodeSub=-1; m_nodeIndex=-1; m_nodes.clear(); resetCloneStroke(); resetMaskStroke();
    if (t==Tool::Node && !m_sel.isEmpty()) beginNodeEdit(m_sel.first());  // show handles right away
    static const Qt::CursorShape cur[] = {
        /*Select*/Qt::ArrowCursor, /*Node*/Qt::ArrowCursor, /*Rect*/Qt::CrossCursor, /*Ellipse*/Qt::CrossCursor, /*Polygon*/Qt::CrossCursor,
        /*Pen*/Qt::CrossCursor, /*Line*/Qt::CrossCursor, /*Freehand*/Qt::CrossCursor, /*Text*/Qt::IBeamCursor, /*Fill*/Qt::CrossCursor,
        /*Transparency*/Qt::CrossCursor, /*Shadow*/Qt::CrossCursor, /*Contour*/Qt::CrossCursor, /*Blend*/Qt::CrossCursor, /*Mould*/Qt::SizeAllCursor,
        /*Zoom*/Qt::PointingHandCursor, /*Pan*/Qt::OpenHandCursor, /*Bevel*/Qt::CrossCursor, /*Feather*/Qt::CrossCursor, /*Eraser*/Qt::CrossCursor,
        /*Extrude*/Qt::CrossCursor, /*Clone*/Qt::CrossCursor };
    static_assert(sizeof(cur)/sizeof(cur[0]) == int(Tool::Clone)+1, "cursor table must cover every Tool enum value");
    if (t==Tool::Mould && !m_sel.isEmpty()) initMould(m_sel.first());
    const int ti=int(t); setCursor(cur[(ti>=0 && ti < int(sizeof(cur)/sizeof(cur[0]))) ? ti : 0]);   // bounds-safe (enum grew past the old 17-entry table)
    emit toolChanged(t); update();
}

void Canvas::setCurrentFill(const Fill& f) {
    m_fill = f;
    if (!m_sel.isEmpty()) applyToSelection([&](Shape& s){ s.fill = f; }, "Fill");
}
void Canvas::setCurrentStroke(const Stroke& s) {
    m_stroke = s;
    if (!m_sel.isEmpty()) applyToSelection([&](Shape& sh){ sh.stroke = s; }, "Stroke");
}

Shape Canvas::styledShape() const {
    Shape s; s.fill = m_fill; s.stroke = m_stroke; return s;
}

void Canvas::setGradHandleColor(const QColor& c){
    if(!c.isValid()) return;
    m_gradHandleColor=c;
    QSettings("XarQ","XarQ").setValue("gradHandleColor", c.name(QColor::HexArgb));
    update();
}

// Build the in-progress / final pen path from anchors and their out-handles.
// The incoming handle of each anchor is the mirror of its out-handle (smooth point);
// a corner point has its handle == the anchor itself, which degenerates to a line.
QPainterPath Canvas::buildPenPath(bool close) const {
    QPainterPath p; if (m_penAnchors.isEmpty()) return p;
    auto seg=[&](int i0,int i1){ QPointF a1=m_penAnchors[i1];
        QPointF h0=m_penHandles[i0]; QPointF h1=a1*2.0 - m_penHandles[i1];
        p.cubicTo(h0,h1,a1); };
    if (m_penResume>=0 && !m_penResumeBase.isEmpty()) {
        // Resuming: keep the existing path EXACTLY and append the new segments from its end node.
        // m_penAnchors[0] is that end node, so append segments 1..N onto the original path.
        p = m_penResumeBase;
        for (int i=1;i<m_penAnchors.size();++i) seg(i-1,i);
        if (close && m_penAnchors.size()>1) p.closeSubpath();
        return p;
    }
    p.moveTo(m_penAnchors[0]);
    for (int i=1;i<m_penAnchors.size();++i) seg(i-1,i);
    if (close && m_penAnchors.size()>2){ seg(m_penAnchors.size()-1,0); p.closeSubpath(); }
    return p;
}
void Canvas::commitPen(bool close){
    if (m_penResume>=0 && m_penResume<m_doc.shapes.size()){
        if (m_penAnchors.size()>=2){          // at least one new segment beyond the resumed end node
            pushUndo("Extend path");
            Shape& s=m_doc.shapes[m_penResume];
            s.path = s.transform.inverted().map(buildPenPath(close));   // buildPenPath is doc-space; store local
            if(close) s.fill = m_fill;
            m_sel={m_penResume}; emit documentChanged(); emit selectionChanged();
        }
    } else if (m_penAnchors.size()>=2){
        Shape s=styledShape(); s.type=ShapeType::Path; s.path=buildPenPath(close);
        if(!close) s.fill.type=FillType::None; s.name="Path"; commitNewShape(s,"Pen");
    }
    m_penActive=false; m_penHandleDrag=false; m_penResume=-1; m_penResumeBase=QPainterPath();
    m_penAnchors.clear(); m_penHandles.clear(); update();
}
// Click near the first/last node of a selected (or hit) OPEN path? => that path can be resumed.
bool Canvas::penHitEndpoint(const QPointF& docPt, int& shapeIdx, bool& atStart) const {
    const double tol = 9.0 / qMax(0.05, m_zoom);        // 9 view px in doc units
    auto consider=[&](int i)->bool{
        if(i<0||i>=m_doc.shapes.size()) return false; const Shape& s=m_doc.shapes[i];
        if(s.type!=ShapeType::Path || s.locked || !s.visible) return false;
        const QPainterPath dp=s.transform.map(s.localPath());
        if(dp.elementCount()<2) return false;
        // open = last element is not a close and the endpoints differ
        const QPointF a=QPointF(dp.elementAt(0)), b=QPointF(dp.elementAt(dp.elementCount()-1));
        if(QLineF(a,b).length()<1e-3) return false;      // closed loop
        if(QLineF(docPt,b).length()<=tol){ shapeIdx=i; atStart=false; return true; }
        if(QLineF(docPt,a).length()<=tol){ shapeIdx=i; atStart=true;  return true; }
        return false; };
    for(int i: m_sel) if(consider(i)) return true;       // prefer a selected path
    for(int i=m_doc.shapes.size()-1;i>=0;--i) if(consider(i)) return true;
    return false;
}

// Start editing a shape's path nodes. We only *parse* its outline here (no mutation),
// so merely picking the Node tool and hovering shows the handles without converting a
// rect/ellipse into a path. The bake to an editable Path happens on the first edit.
void Canvas::beginNodeEdit(int shapeIdx) {
    m_nodeSelSet.clear();   // node indices become stale on a re-parse / shape switch
    m_nodeChild=-1;
    if (shapeIdx<0 || shapeIdx>=m_doc.shapes.size()) { m_nodeShape=-1; m_nodes.clear(); return; }
    m_nodes=pathToNodes(m_doc.shapes[shapeIdx].localPath());
    m_nodeShape=shapeIdx;
}
// Enter a group to edit one child path's nodes without ungrouping (node-edit-in-group).
void Canvas::beginNodeEditChild(int groupIdx, int childIdx) {
    m_nodeSelSet.clear();
    if(groupIdx<0||groupIdx>=m_doc.shapes.size()) { m_nodeShape=-1; m_nodeChild=-1; m_nodes.clear(); return; }
    Shape& g=m_doc.shapes[groupIdx];
    if(g.type!=ShapeType::Group || childIdx<0 || childIdx>=g.children.size()){ beginNodeEdit(groupIdx); return; }
    m_nodeShape=groupIdx; m_nodeChild=childIdx;
    m_nodes=pathToNodes(g.children[childIdx].localPath());
}
Shape* Canvas::nodeTargetShape() {
    if(m_nodeShape<0||m_nodeShape>=m_doc.shapes.size()) return nullptr;
    Shape& g=m_doc.shapes[m_nodeShape];
    if(m_nodeChild>=0 && g.type==ShapeType::Group && m_nodeChild<g.children.size()) return &g.children[m_nodeChild];
    return &g;
}
QTransform Canvas::nodeXf() const {
    if(m_nodeShape<0||m_nodeShape>=m_doc.shapes.size()) return QTransform();
    const Shape& g=m_doc.shapes[m_nodeShape];
    if(m_nodeChild>=0 && g.type==ShapeType::Group && m_nodeChild<g.children.size()) return g.transform * g.children[m_nodeChild].transform;
    return g.transform;
}
void Canvas::writeBackNodes() {
    Shape* t=nodeTargetShape(); if(!t) return;
    t->type=ShapeType::Path;                    // bake on first edit (undo snapshot predates this)
    t->path=nodesToPath(m_nodes);
    emit documentChanged();
}

// ---- selection helpers -----------------------------------------------------
Shape* Canvas::firstSelected() {
    if (inGroup()) { Shape* c=enteredContainer(); if(c && !m_subSel.isEmpty() && m_subSel.first()<c->children.size()) return &c->children[m_subSel.first()]; return nullptr; }
    return m_sel.isEmpty()? nullptr : &m_doc.shapes[m_sel.first()];
}

// ----- Select-into-group descent ------------------------------------------------
Shape* Canvas::enteredContainer() {
    if (m_enterPath.isEmpty() || m_enterPath[0]<0 || m_enterPath[0]>=m_doc.shapes.size()) return nullptr;
    Shape* s=&m_doc.shapes[m_enterPath[0]];
    for (int k=1;k<m_enterPath.size();++k){ if(s->type!=ShapeType::Group || m_enterPath[k]<0 || m_enterPath[k]>=s->children.size()) return nullptr; s=&s->children[m_enterPath[k]]; }
    return s->type==ShapeType::Group ? s : nullptr;
}
const Shape* Canvas::enteredContainer() const { return const_cast<Canvas*>(this)->enteredContainer(); }
QTransform Canvas::containerXf() const {
    QTransform t;
    if (m_enterPath.isEmpty() || m_enterPath[0]<0 || m_enterPath[0]>=m_doc.shapes.size()) return t;
    const Shape* s=&m_doc.shapes[m_enterPath[0]]; t=s->transform;
    for (int k=1;k<m_enterPath.size();++k){ if(s->type!=ShapeType::Group || m_enterPath[k]<0 || m_enterPath[k]>=s->children.size()) break; s=&s->children[m_enterPath[k]]; t=t*s->transform; }
    return t;
}
QVector<Shape*> Canvas::selTargets() {
    QVector<Shape*> v;
    if (inGroup()) { Shape* c=enteredContainer(); if(c) for(int ci:m_subSel) if(ci>=0 && ci<c->children.size()) v.push_back(&c->children[ci]); }
    else for(int i:m_sel) if(i>=0 && i<m_doc.shapes.size()) v.push_back(&m_doc.shapes[i]);
    return v;
}
void Canvas::enterGroup(int topIndex, int childHit) {
    if (topIndex<0 || topIndex>=m_doc.shapes.size() || m_doc.shapes[topIndex].type!=ShapeType::Group) return;
    resetTextEdit(); m_rotateMode=false; m_rotPivotSet=false;
    m_enterPath = {topIndex};
    m_sel = {topIndex};                               // keep the group selected at top level (menu/structural ops stay safe)
    m_subSel.clear(); if(childHit>=0) m_subSel = {childHit};
    emit selectionChanged(); update();
}
void Canvas::exitGroup() {
    if (m_enterPath.isEmpty()) return;
    const int top = m_enterPath.first();
    m_enterPath.removeLast();
    if (m_enterPath.isEmpty()) { m_sel = (top>=0 && top<m_doc.shapes.size()) ? QVector<int>{top} : QVector<int>{}; m_subSel.clear(); }
    else { m_subSel.clear(); }                        // popped to an outer group level; nothing sub-selected yet
    emit selectionChanged(); update();
}
void Canvas::resetGroupEntry() { m_enterPath.clear(); m_subSel.clear(); }

QRectF Canvas::selBoundsDoc() const {
    QRectF b;
    if (inGroup()) {
        const Shape* c=enteredContainer(); if(!c) return b; const QTransform cx=containerXf();
        for (int ci:m_subSel){ if(ci<0||ci>=c->children.size())continue;
            QRectF db=(cx*c->children[ci].transform).map(c->children[ci].localPath()).boundingRect();
            b=b.isNull()?db:b.united(db); }
        return b;
    }
    for (int i : m_sel) { QRectF db=m_doc.shapes[i].docBounds(); b=b.isNull()?db:b.united(db); }
    return b;
}
// Build the doc-space selection silhouette + bounds that drawSelection strokes, but only when the cache is stale.
// This is the ONLY place that recurses a group's full geometry for the overlay — reused across every repaint until
// the selection/doc changes, so a per-frame repaint (e.g. sweeping the Ctrl+Alt pie) no longer rebuilds it each frame.
// Cheap recursive SHAPE COUNT — a pure pointer walk, builds no paths. Used to decide whether a selection is small
// enough to build the exact (merged, AA-stroked) silhouette or must fall back to bbox-only.
static int countShapesRec(const Shape& s){
    if (s.type != ShapeType::Group) return 1;
    int n=1; for (const Shape& c : s.children) n += countShapesRec(c); return n;
}
// Doc-space bounds of a shape via child-RECT unions — never builds one giant merged QPainterPath. A Group's
// Shape::localPath()/docBounds() recurse the whole subtree into a single enormous path (then boundingRect it); for a
// 35-page imported group that merge alone is a multi-second stall. This unions leaf bounding rects instead.
static QRectF cheapLocalBounds(const Shape& s){
    if (s.type != ShapeType::Group) return s.localBounds();      // leaf: its own small path's bbox
    QRectF b;
    for (const Shape& c : s.children){ QRectF cb = c.transform.mapRect(cheapLocalBounds(c)); b = b.isNull()? cb : b.united(cb); }
    return b;
}
static const int kSelOutlineShapeCap = 400;   // above this many shapes, skip the merged/AA-stroked silhouette — bbox + handles only

void Canvas::ensureSelOutlineCache(){
    // A live manipulation moves the geometry every frame, so never serve a stale outline during one — always rebuild
    // (the non-drag idle/pie-hover case is where caching matters, and there manipulatingContent() is false).
    if (m_selCacheValid && !manipulatingContent()) return;
    m_selOutlineDoc = QPainterPath();
    if (inGroup()) {
        // Dashed "you are inside this group" boundary = the whole container silhouette (expensive) — build it only if
        // the container is small enough. The HANDLE BOX is the sub-selection bounds (usually one child → cheap + exact).
        const Shape* c=enteredContainer();
        if (c && countShapesRec(*c) <= kSelOutlineShapeCap) m_selOutlineDoc = containerXf().map(c->localPath());
        m_selBoundsDoc = selBoundsDoc();
    } else {
        int count=0; for (int i : m_sel) if (i>=0 && i<m_doc.shapes.size()) count += countShapesRec(m_doc.shapes[i]);
        if (count <= kSelOutlineShapeCap) {                         // light: exact silhouette + exact bounds (no regression)
            for (int i : m_sel) if (i>=0 && i<m_doc.shapes.size()) m_selOutlineDoc.addPath(Document::docPath(m_doc.shapes[i]));
            m_selBoundsDoc = m_selOutlineDoc.boundingRect();
        } else {                                                    // heavy: no giant merge — union cheap leaf rects for the box
            QRectF b; for (int i : m_sel) if (i>=0 && i<m_doc.shapes.size()){ const Shape& s=m_doc.shapes[i];
                QRectF db = s.transform.mapRect(cheapLocalBounds(s)); b = b.isNull()? db : b.united(db); }
            m_selBoundsDoc = b;
        }
    }
    m_selCacheValid = !manipulatingContent();          // don't persist a transient mid-manipulation snapshot
}
void Canvas::select(const QVector<int>& idx){ resetGroupEntry(); m_sel=idx; m_rotateMode=false; m_rotPivotSet=false; emit selectionChanged(); update(); }
void Canvas::selectAll(){ m_sel.clear(); for(int i=0;i<m_doc.shapes.size();++i) m_sel.push_back(i); emit selectionChanged(); update(); }
void Canvas::selectNone(){ if(m_sel.isEmpty())return; m_sel.clear(); emit selectionChanged(); update(); }

void Canvas::pushUndo(const QString& label) {
    m_cacheDirty = true;   // a mutation is about to happen (belt-and-suspenders with the documentChanged hook)
    m_undo.push_back(captureSnap());   // COW snapshot: ~free here, detaches lazily on the coming mutation
    m_undoLabels.push_back(label);
    if (m_undo.size() > 200) { m_undo.removeFirst(); m_undoLabels.removeFirst(); }
    m_redo.clear(); m_redoLabels.clear();
}
Canvas::UndoSnap Canvas::captureSnap() const {
    UndoSnap s{ m_doc, m_sel };
    // Give the snapshot its OWN copy of the mutable per-object containers, leaving m_doc's storage (and any
    // live `Shape& s = m_doc.shapes[i]` references held across pushUndo) pointing at the original. Several edit
    // paths grab such a reference BEFORE pushUndo and mutate through it afterwards (replacePhotoAt, text/cell/
    // mask edits); without this, that write would bypass copy-on-write and corrupt the shared snapshot. The
    // detach is an O(N) shallow copy of the struct array (~2 ms for 3000 objects) — still ~30× cheaper than the
    // old whole-document JSON serialize, and each Shape's own image/text/path stays COW-shared until it changes.
    s.doc.shapes.detach();
    s.doc.layers.detach();
    return s;
}
void Canvas::applyUndoSnap(const UndoSnap& s) {
    m_doc = s.doc;
    m_sel.clear(); for (int i : s.sel) if(i>=0 && i<m_doc.shapes.size()) m_sel.push_back(i);
    m_nodeShape=-1; m_nodeChild=-1; m_nodeSub=-1; m_nodeIndex=-1; m_nodes.clear();  // path may have changed; re-parse on next edit
    resetCloneState(); resetMaskStroke();   // undo/redo restored a different doc state; clone/mask state may be stale
}
void Canvas::undo(){ if(m_undo.size()<1)return; if(m_cropMode) cancelCropMode(); resetGroupEntry(); m_redo.push_back(captureSnap()); m_redoLabels.push_back(m_undoLabels.isEmpty()?QString():m_undoLabels.takeLast()); applyUndoSnap(m_undo.takeLast()); syncTextEditToDoc(); emit selectionChanged(); emit documentChanged(); update(); }
void Canvas::redo(){ if(m_redo.isEmpty())return; if(m_cropMode) cancelCropMode(); resetGroupEntry(); m_undo.push_back(captureSnap()); m_undoLabels.push_back(m_redoLabels.isEmpty()?QString():m_redoLabels.takeLast()); applyUndoSnap(m_redo.takeLast()); syncTextEditToDoc(); emit selectionChanged(); emit documentChanged(); update(); }

// Round every corner of a closed/open polyline: inset by `r` along both edges and arc through the vertex.
static QPainterPath roundPolyCorners(const QPolygonF& poly, bool closed, double r) {
    QPainterPath p; const int n=poly.size();
    if (n<3 || r<=0){ p.addPolygon(poly); if(closed) p.closeSubpath(); return p; }
    auto V=[&](int i){ return poly[(i%n+n)%n]; };
    auto unit=[](QPointF v){ double l=std::hypot(v.x(),v.y()); return l>1e-9? v/l : QPointF(); };
    const int first = closed?0:1, last = closed?n:n-1;   // don't round open-path endpoints
    if(!closed) p.moveTo(poly.first());
    for (int i=first;i<last;++i){
        QPointF cur=V(i);
        double l1=std::hypot((V(i-1)-cur).x(),(V(i-1)-cur).y()), l2=std::hypot((V(i+1)-cur).x(),(V(i+1)-cur).y());
        QPointF d1=unit(V(i-1)-cur), d2=unit(V(i+1)-cur);
        double rr=qMin(r, qMin(l1,l2)*0.5);
        QPointF a=cur+d1*rr, b=cur+d2*rr;
        if (i==first && closed) p.moveTo(a); else p.lineTo(a);
        p.quadTo(cur, b);
    }
    if (closed) p.closeSubpath(); else p.lineTo(poly.last());
    return p;
}
void Canvas::roundSelectionCorners(double radius) {
    if (m_sel.isEmpty() || radius<=0) return;
    pushUndo("Round corners");
    for (int i : m_sel) { Shape& s=m_doc.shapes[i];
        if (s.type==ShapeType::Rect){ s.cornerRadius=radius; continue; }   // rects have a native rounded corner
        QPainterPath src;
        if (s.type==ShapeType::Path) src=s.path;
        else if (s.type==ShapeType::Polygon) src=s.localPath();
        else continue;
        QPainterPath out;
        for (const QPolygonF& poly : src.toSubpathPolygons()) {
            const bool cl=poly.isClosed();
            out.addPath(roundPolyCorners(cl ? QPolygonF(poly.mid(0,poly.size()-1)) : poly, cl, radius));
        }
        s.type=ShapeType::Path; s.path=out;
    }
    emit documentChanged(); emit selectionChanged(); update();
}
QColor Canvas::sampleColorAt(const QPointF& docPt) const {
    // Render the composited document into a 1×1 buffer positioned so docPt lands at (0,0).
    QImage img(1,1,QImage::Format_ARGB32); img.fill(Qt::white);
    { QPainter g(&img); g.setRenderHint(QPainter::Antialiasing);
      g.translate(-docPt.x(), -docPt.y()); paintDocument(g, m_doc); }
    return img.pixelColor(0,0);
}
void Canvas::skewSelection(double hDeg, double vDeg) {
    if (m_sel.isEmpty() || (qAbs(hDeg)<1e-9 && qAbs(vDeg)<1e-9)) return;
    const QPointF c = selBoundsDoc().center();
    const double sh=std::tan(hDeg*M_PI/180.0), sv=std::tan(vDeg*M_PI/180.0);
    QTransform shear; shear.translate(c.x(),c.y()); shear.shear(sh,sv); shear.translate(-c.x(),-c.y());
    pushUndo("Skew");
    for (int i:m_sel) m_doc.shapes[i].transform = m_doc.shapes[i].transform * shear;  // apply oldXf then doc-space shear
    emit documentChanged(); emit selectionChanged(); update();
}
int Canvas::nodeSubpaths() const { return m_nodes.size(); }
int Canvas::nodeCount(int sub) const { return (sub>=0 && sub<m_nodes.size()) ? m_nodes[sub].nodes.size() : 0; }
void Canvas::deleteNode(int sub, int idx) {
    if (m_nodeShape<0 || sub<0 || sub>=m_nodes.size()) return;
    SubPath& sp=m_nodes[sub]; if (idx<0 || idx>=sp.nodes.size()) return;
    pushUndo("Delete node");
    sp.nodes.removeAt(idx);
    if (sp.nodes.size()<2) m_nodes.removeAt(sub);          // a subpath needs ≥2 nodes
    m_nodeSub=-1; m_nodeIndex=-1;
    writeBackNodes(); update();
}
// Geometry only (no undo / write-back) so it can be applied to one node or a whole selection in one step.
bool Canvas::applyNodeCurve(int sub, int idx, bool curved) {
    if (m_nodeShape<0 || sub<0 || sub>=m_nodes.size()) return false;
    SubPath& sp=m_nodes[sub]; if (idx<0 || idx>=sp.nodes.size()) return false;
    PathNode& n=sp.nodes[idx];
    if (!curved) { n.hasIn=n.hasOut=false; n.smooth=false; }   // straight segments into/out of this node
    else {                                                     // smooth bezier tangent from the neighbours
        const QPointF prev = idx>0 ? sp.nodes[idx-1].anchor : (sp.closed? sp.nodes.last().anchor : n.anchor);
        const QPointF next = idx<sp.nodes.size()-1 ? sp.nodes[idx+1].anchor : (sp.closed? sp.nodes.first().anchor : n.anchor);
        QPointF dir=next-prev; const double len=std::hypot(dir.x(),dir.y());
        // Straight-neighbour fallback so an end node / degenerate corner still gets grabbable handles.
        double d = QLineF(prev,next).length()/6.0;
        if (len<=1e-3 || d<4.0){ dir=QPointF(1,0); d=qMax(24.0, QLineF(prev,next).length()/6.0); }
        else dir/=len;
        n.in=n.anchor-dir*d; n.out=n.anchor+dir*d; n.hasIn=n.hasOut=true; n.smooth=true;
    }
    return true;
}
void Canvas::setNodeCurved(int sub, int idx, bool curved) {
    if (m_nodeShape<0) return;
    pushUndo(curved?"Make curve":"Make line");
    if (applyNodeCurve(sub, idx, curved)) { writeBackNodes(); update(); }
}
void Canvas::insertNodeOnSegment(int sub, int idx, double t) {
    if (m_nodeShape<0 || sub<0 || sub>=m_nodes.size()) return;
    SubPath& sp=m_nodes[sub]; const int n=sp.nodes.size();
    if (idx<0 || idx>=n) return;
    int j=idx+1; if (j>=n){ if(sp.closed) j=0; else return; }
    t=qBound(0.0,t,1.0);
    PathNode& A=sp.nodes[idx]; PathNode& B=sp.nodes[j];
    const bool cubic = A.hasOut || B.hasIn;
    PathNode N;
    if (!cubic) { N.anchor = A.anchor*(1.0-t) + B.anchor*t; N.hasIn=N.hasOut=false; N.smooth=false; }
    else {   // de Casteljau split of the cubic (a straight seg is a cubic with p1=p0, p2=p3)
        const QPointF p0=A.anchor, p1=A.hasOut?A.out:A.anchor, p2=B.hasIn?B.in:B.anchor, p3=B.anchor;
        const QPointF a=p0*(1.0-t)+p1*t, b=p1*(1.0-t)+p2*t, cc=p2*(1.0-t)+p3*t;
        const QPointF dd=a*(1.0-t)+b*t, ee=b*(1.0-t)+cc*t;
        N.anchor = dd*(1.0-t)+ee*t; N.in=dd; N.out=ee; N.hasIn=N.hasOut=true; N.smooth=true;
        A.out=a; A.hasOut=true; B.in=cc; B.hasIn=true;
    }
    pushUndo("Add node");
    sp.nodes.insert(idx+1, N);
    m_nodeSub=sub; m_nodeIndex=idx+1;
    writeBackNodes(); update();
}
void Canvas::reshapePathSegment(int sub, int segIdx, double t, const QPointF& out0, const QPointF& in0,
                                const QPointF& grabLocal, const QPointF& targetLocal) {
    if (m_nodeShape<0 || sub<0 || sub>=m_nodes.size()) return;
    SubPath& sp=m_nodes[sub]; const int n=sp.nodes.size();
    if (segIdx<0 || segIdx>=n) return;
    int j=segIdx+1; if (j>=n){ if(sp.closed) j=0; else return; }
    t=qBound(0.0,t,1.0);
    // The curve point at t is P = a·A + w1·out + w2·in + b·B (a,b anchor weights). Moving `out` and `in`
    // shifts P by w1·Δout + w2·Δin; the minimal (least-squares) way to move P by `delta` is
    // Δout = delta·w1/(w1²+w2²), Δin = delta·w2/(w1²+w2²). A straight segment's out0/in0 are its
    // 1/3–2/3 points, so this promotes it to a curve.
    const double u=1.0-t, w1=3.0*u*u*t, w2=3.0*u*t*t;
    double denom=w1*w1+w2*w2; if(denom<1e-9) return;   // t at an endpoint: nothing to bend
    // Cap each handle's response so grabbing NEAR (but not at) an endpoint — where w1/denom ≈ 1/(3t) blows
    // up — gives a gentle local bend instead of a huge loop. Interior grabs (factor ≲ 1.5) are unaffected,
    // so P(t) still lands exactly on the cursor there.
    const double cap=4.0;
    const double f1=qMin(w1/denom, cap), f2=qMin(w2/denom, cap);
    const QPointF delta=targetLocal-grabLocal;
    PathNode& A=sp.nodes[segIdx]; PathNode& B=sp.nodes[j];
    A.out = out0 + delta*f1; A.hasOut=true;
    B.in  = in0  + delta*f2; B.hasIn =true;
    // Bending one segment shouldn't ripple to the neighbouring segment via a smooth node's mirror, so the
    // touched anchors become cusps here (their other handle is untouched); re-smooth with a double-click.
    A.smooth=false; B.smooth=false;
    writeBackNodes();
}
void Canvas::deleteActiveNode()          { if (m_nodeSub>=0 && m_nodeIndex>=0) deleteNode(m_nodeSub, m_nodeIndex);
                                           else if (!m_nodeSelSet.isEmpty()) deleteSelectedNodes(); }  // InfoBar (post-release): use the selection
void Canvas::selectNodesInRect(const QRectF& docRect) {
    m_nodeSelSet.clear();
    if (m_nodeShape<0) return; const QTransform xf=nodeXf();
    for (int si=0;si<m_nodes.size();++si) for (int ni=0;ni<m_nodes[si].nodes.size();++ni)
        if (docRect.contains(xf.map(m_nodes[si].nodes[ni].anchor))) m_nodeSelSet.push_back(qMakePair(si,ni));
    update();
}
void Canvas::deleteSelectedNodes() {
    if (m_nodeShape<0 || m_nodeSelSet.isEmpty()) return;
    QMap<int,QVector<int>> bySub; for (const auto& pr:m_nodeSelSet) bySub[pr.first].push_back(pr.second);
    pushUndo("Delete nodes");
    QList<int> subs=bySub.keys(); std::sort(subs.begin(),subs.end(),std::greater<int>());   // high subpaths first
    for (int su:subs){ if(su>=m_nodes.size()) continue; QVector<int> idxs=bySub[su]; std::sort(idxs.begin(),idxs.end(),std::greater<int>());
        for (int id:idxs) if(id<m_nodes[su].nodes.size()) m_nodes[su].nodes.removeAt(id);
        if (m_nodes[su].nodes.size()<2) m_nodes.removeAt(su); }
    m_nodeSelSet.clear(); m_nodeSub=-1; m_nodeIndex=-1; writeBackNodes(); update();
}

// Reverse the direction of the edited subpath(s): flip node order + swap each node's in/out handle.
// Affects the subpaths that hold selected/grabbed nodes, or the whole path if nothing is selected.
void Canvas::reverseNodePath() {
    if (m_nodeShape<0 || m_nodes.isEmpty()) return;
    QSet<int> subs; for (const auto& pr : m_nodeSelSet) subs.insert(pr.first);
    if (subs.isEmpty() && m_nodeSub>=0) subs.insert(m_nodeSub);
    if (subs.isEmpty()) for (int i=0;i<m_nodes.size();++i) subs.insert(i);
    pushUndo("Reverse path");
    for (int su : subs){ if(su<0||su>=m_nodes.size()) continue;
        auto& ns=m_nodes[su].nodes; std::reverse(ns.begin(), ns.end());
        for (auto& n : ns){ std::swap(n.in,n.out); std::swap(n.hasIn,n.hasOut); } }
    m_nodeSub=-1; m_nodeIndex=-1; writeBackNodes(); update();
    emit statusMessage("Path direction reversed.");
}

// Break at points: at the (lowest) selected node of each affected subpath, split the path so the node
// becomes two coincident endpoints — a closed path opens there; an open path splits into two subpaths.
void Canvas::breakSelectedNodes() {
    if (m_nodeShape<0) return;
    QMap<int,int> firstIdx;  // subpath → lowest selected node index in it
    for (const auto& pr : m_nodeSelSet){ if(!firstIdx.contains(pr.first) || pr.second<firstIdx[pr.first]) firstIdx[pr.first]=pr.second; }
    if (firstIdx.isEmpty() && m_nodeSub>=0 && m_nodeIndex>=0) firstIdx[m_nodeSub]=m_nodeIndex;
    if (firstIdx.isEmpty()) { emit statusMessage("Select a node to break the path at."); return; }
    pushUndo("Break at points");
    bool changed=false;
    QList<int> subs=firstIdx.keys(); std::sort(subs.begin(),subs.end(),std::greater<int>());   // high subpaths first (inserts don't shift lower ones)
    for (int su : subs){ if(su<0||su>=m_nodes.size()) continue; int k=firstIdx[su];
        auto& sp=m_nodes[su]; int n=sp.nodes.size(); if(k<0||k>=n) continue;
        if (sp.closed){                              // open the closed path at node k
            QVector<xara::PathNode> ns; for(int i=0;i<n;++i) ns.push_back(sp.nodes[(k+i)%n]);
            xara::PathNode endCopy=sp.nodes[k]; endCopy.hasOut=false;
            ns.first().hasIn=false; ns.push_back(endCopy);
            sp.nodes=ns; sp.closed=false; changed=true;
        } else if (k>0 && k<n-1){                    // split an open path into two open subpaths
            xara::SubPath a,b; a.closed=false; b.closed=false;
            for(int i=0;i<=k;++i) a.nodes.push_back(sp.nodes[i]);
            for(int i=k;i<n;++i) b.nodes.push_back(sp.nodes[i]);
            a.nodes.last().hasOut=false; b.nodes.first().hasIn=false;
            m_nodes[su]=a; m_nodes.insert(su+1,b); changed=true;
        }
    }
    m_nodeSelSet.clear(); m_nodeSub=-1; m_nodeIndex=-1;
    if (changed){ writeBackNodes(); update(); emit statusMessage("Path broken at the selected node(s)."); }
    else emit statusMessage("Nothing to break (a path endpoint stays as-is).");
}

// Join two selected end nodes: weld two open-path endpoints into one continuous path (or close a path
// when both ends of the SAME open subpath are selected).
void Canvas::joinSelectedNodes() {
    if (m_nodeShape<0 || m_nodeSelSet.size()!=2){ emit statusMessage("Select exactly two path end nodes to join."); return; }
    auto endKind=[&](int su,int idx)->int{ if(su<0||su>=m_nodes.size()||m_nodes[su].closed) return -1;
        int n=m_nodes[su].nodes.size(); if(idx==0) return 0; if(idx==n-1) return 1; return -1; };
    int su1=m_nodeSelSet[0].first, i1=m_nodeSelSet[0].second, su2=m_nodeSelSet[1].first, i2=m_nodeSelSet[1].second;
    int e1=endKind(su1,i1), e2=endKind(su2,i2);
    if (e1<0||e2<0){ emit statusMessage("Join needs two open-path END nodes."); return; }
    pushUndo("Join nodes");
    auto rev=[](QVector<xara::PathNode>& v){ std::reverse(v.begin(),v.end()); for(auto&n:v){std::swap(n.in,n.out);std::swap(n.hasIn,n.hasOut);} };
    if (su1==su2){ m_nodes[su1].closed=true; }      // two ends of one path → close it
    else {
        QVector<xara::PathNode> a=m_nodes[su1].nodes, b=m_nodes[su2].nodes;
        if (e1==0) rev(a);                           // a must END at its join node
        if (e2==1) rev(b);                           // b must START at its join node
        if (!a.isEmpty() && !b.isEmpty()){
            a.last().anchor=(a.last().anchor+b.first().anchor)/2.0;   // weld coincident endpoints
            a.last().hasOut=b.first().hasOut; a.last().out=b.first().out;
            for(int k=1;k<b.size();++k) a.push_back(b[k]); }
        m_nodes[su1].nodes=a; m_nodes[su1].closed=false; m_nodes.removeAt(su2);
    }
    m_nodeSelSet.clear(); m_nodeSub=-1; m_nodeIndex=-1; writeBackNodes(); update();
    emit statusMessage("Nodes joined.");
}
void Canvas::setActiveNodeCurved(bool c) {
    // A grabbed node (mid-drag) takes priority; otherwise (e.g. the InfoBar "Make Curve" button, clicked
    // AFTER the mouse was released so m_nodeSub was reset) apply to the persistent selection set.
    if (m_nodeSub>=0 && m_nodeIndex>=0) { setNodeCurved(m_nodeSub, m_nodeIndex, c); return; }
    if (m_nodeShape<0 || m_nodeSelSet.isEmpty()) return;
    pushUndo(c?"Make curve":"Make line");
    bool any=false; for (const auto& pr : m_nodeSelSet) any = applyNodeCurve(pr.first, pr.second, c) || any;
    if (any) { writeBackNodes(); update(); }
}
static void setNodeContinuity(PathNode& nd, int type);   // fwd (defined with the node helpers below)
void Canvas::setSelectedNodeType(int type) {
    // Cusp / Smooth / Symmetric on the grabbed node, else the persistent selection set (InfoBar buttons).
    if (m_nodeShape<0) return;
    auto at=[&](int sub,int idx)->PathNode*{ if(sub<0||sub>=m_nodes.size())return nullptr; SubPath&sp=m_nodes[sub];
        return (idx<0||idx>=sp.nodes.size())?nullptr:&sp.nodes[idx]; };
    if (m_nodeSub>=0 && m_nodeIndex>=0) { if(PathNode* n=at(m_nodeSub,m_nodeIndex)){ pushUndo("Node type"); setNodeContinuity(*n,type); writeBackNodes(); update(); } return; }
    if (m_nodeSelSet.isEmpty()) return;
    pushUndo("Node type"); bool any=false;
    for (const auto& pr : m_nodeSelSet) if(PathNode* n=at(pr.first,pr.second)){ setNodeContinuity(*n,type); any=true; }
    if (any) { writeBackNodes(); update(); }
}
void Canvas::autoFitToPage(bool bothAxes) {
    if (m_doc.shapes.isEmpty()) return;
    const QRectF cb = m_doc.contentBounds(); if (cb.width()<1e-6 || cb.height()<1e-6) return;
    const QRectF page = m_doc.pageRect(0);
    const double margin = 0.04;   // leave a 4% border
    const double sx = page.width()*(1-2*margin)/cb.width(), sy = page.height()*(1-2*margin)/cb.height();
    const double s = bothAxes ? qMin(sx,sy) : sx;   // width-only fits the width; both fits inside the page
    if (!(s>0) || !std::isfinite(s)) return;
    pushUndo("Auto-fit to page");
    const QPointF cc=cb.center(), pc=page.center();
    QTransform t; t.translate(pc.x(),pc.y()); t.scale(s,s); t.translate(-cc.x(),-cc.y());
    for (Shape& sh : m_doc.shapes) sh.transform = sh.transform * t;
    emit documentChanged(); emit selectionChanged(); update();
}
void Canvas::setShapeRegion(int shapeIdx, const QPainterPath& localRegion) {
    if (shapeIdx<0 || shapeIdx>=m_doc.shapes.size() || localRegion.isEmpty()) return;
    pushUndo("Photo region"); m_doc.shapes[shapeIdx].regionPath=localRegion;
    emit documentChanged(); emit selectionChanged(); update();
}
void Canvas::clearShapeRegion(int shapeIdx) {
    if (shapeIdx<0 || shapeIdx>=m_doc.shapes.size() || m_doc.shapes[shapeIdx].regionPath.isEmpty()) return;
    pushUndo("Clear region"); m_doc.shapes[shapeIdx].regionPath=QPainterPath();
    emit documentChanged(); emit selectionChanged(); update();
}

// Livewire / Intelligent-Scissors: least-cost 8-connected path from a→b over a per-pixel cost map
// (strong edges cost less, so the path hugs them). Restricted to a padded bbox around the endpoints.
static QVector<QPoint> livewirePath(const std::vector<float>& cost, int W, int H, QPoint a, QPoint b){
    const int pad=48;
    const int x0=qMax(0,qMin(a.x(),b.x())-pad), x1=qMin(W-1,qMax(a.x(),b.x())+pad);
    const int y0=qMax(0,qMin(a.y(),b.y())-pad), y1=qMin(H-1,qMax(a.y(),b.y())+pad);
    const int bw=x1-x0+1, bh=y1-y0+1; if(bw<1||bh<1) return {a,b};
    auto idx=[&](int x,int y){ return (y-y0)*bw+(x-x0); };
    std::vector<float> dist(size_t(bw)*bh, 1e30f); std::vector<int> prev(size_t(bw)*bh,-1);
    typedef std::pair<float,int> QN;
    std::priority_queue<QN,std::vector<QN>,std::greater<QN>> pq;
    const int sa=idx(a.x(),a.y()), tb=idx(b.x(),b.y());
    dist[sa]=0.f; pq.push({0.f,sa});
    const int dxk[8]={1,-1,0,0,1,1,-1,-1}, dyk[8]={0,0,1,-1,1,-1,1,-1};
    while(!pq.empty()){ QN top=pq.top(); pq.pop(); float dd=top.first; int u=top.second;
        if(dd>dist[u]) continue; if(u==tb) break;
        const int ux=x0+u%bw, uy=y0+u/bw;
        for(int k=0;k<8;++k){ const int nx=ux+dxk[k], ny=uy+dyk[k];
            if(nx<x0||nx>x1||ny<y0||ny>y1) continue;
            const int v=idx(nx,ny); const float w=cost[size_t(ny)*W+nx]*((k<4)?1.0f:1.41421356f);
            if(dist[u]+w<dist[v]){ dist[v]=dist[u]+w; prev[v]=u; pq.push({dist[v],v}); } } }
    if(prev[tb]<0 && tb!=sa) return {a,b};   // unreachable (shouldn't happen on a grid): straight fallback
    QVector<QPoint> path; int cur=tb;
    while(cur>=0){ path.prepend(QPoint(x0+cur%bw, y0+cur/bw)); if(cur==sa) break; cur=prev[cur]; }
    return path;
}

// Magnetic Lasso: snap a closed selection through the given doc-space anchor points to the strongest
// nearby edges of the photo, returning the region as a LOCAL-space closed path (empty on failure).
QPainterPath Canvas::magneticLassoRegion(int si, const QVector<QPointF>& docAnchors) const {
    if(si<0 || si>=m_doc.shapes.size() || docAnchors.size()<2) return {};
    const Shape& s=m_doc.shapes[si];
    if(s.fill.type!=FillType::Bitmap || s.fill.image.isNull()) return {};
    const QImage img=s.fill.image.convertToFormat(QImage::Format_ARGB32);
    const int W=img.width(), H=img.height(); if(W<3||H<3) return {};
    // luminance → Sobel gradient magnitude → cost (edge ≈ 1, flat ≈ 9)
    std::vector<float> lum(size_t(W)*H);
    for(int y=0;y<H;++y){ const QRgb* r=(const QRgb*)img.constScanLine(y);
        for(int x=0;x<W;++x){ QRgb p=r[x]; lum[size_t(y)*W+x]=0.299f*qRed(p)+0.587f*qGreen(p)+0.114f*qBlue(p); } }
    std::vector<float> g(size_t(W)*H,0.f), cost(size_t(W)*H,1.f); float gmax=1e-3f;
    for(int y=1;y<H-1;++y) for(int x=1;x<W-1;++x){
        const float gx=lum[size_t(y-1)*W+x+1]+2*lum[size_t(y)*W+x+1]+lum[size_t(y+1)*W+x+1]
                      -lum[size_t(y-1)*W+x-1]-2*lum[size_t(y)*W+x-1]-lum[size_t(y+1)*W+x-1];
        const float gy=lum[size_t(y+1)*W+x-1]+2*lum[size_t(y+1)*W+x]+lum[size_t(y+1)*W+x+1]
                      -lum[size_t(y-1)*W+x-1]-2*lum[size_t(y-1)*W+x]-lum[size_t(y-1)*W+x+1];
        const float m=std::sqrt(gx*gx+gy*gy); g[size_t(y)*W+x]=m; if(m>gmax) gmax=m; }
    for(size_t i=0;i<g.size();++i) cost[i]=1.0f+(gmax-g[i])/gmax*8.0f;
    // doc→image (docToImagePixel) and image→local (tex)
    const double IW=qMax(1,W), IH=qMax(1,H);
    const QPointF ax=(s.fill.p1-s.fill.p0)/IW, ay=(s.fill.p0-s.fill.p2)/IH;
    const QTransform tex(ax.x(),ax.y(),ay.x(),ay.y(),s.fill.p2.x(),s.fill.p2.y());   // image px → local
    QVector<QPoint> ipts;
    for(const QPointF& d: docAnchors){ QPointF ip; if(!docToImagePixel(si,d,ip)) return {};
        ipts.push_back(QPoint(qBound(0,int(ip.x()),W-1), qBound(0,int(ip.y()),H-1))); }
    QVector<QPoint> loop;
    for(int i=0;i<ipts.size();++i){ const QVector<QPoint> seg=livewirePath(cost,W,H,ipts[i],ipts[(i+1)%ipts.size()]);
        for(int j=(i==0?0:1); j<seg.size(); ++j) loop.push_back(seg[j]); }
    if(loop.size()<3) return {};
    QPainterPath lp; bool first=true;
    for(const QPoint& q: loop){ const QPointF lpt=tex.map(QPointF(q.x(),q.y()));
        if(first){ lp.moveTo(lpt); first=false; } else lp.lineTo(lpt); }
    lp.closeSubpath();
    return lp;
}

// Effect Painter: lay a locally-adjusted copy of the photo, clipped to `localRegion`, directly above
// the original — so the chosen enhancement shows ONLY inside the painted area and the base stays intact.
void Canvas::paintEffectRegion(int si, const QPainterPath& localRegion, int kind){
    if(si<0 || si>=m_doc.shapes.size() || localRegion.isEmpty()) return;
    const Shape& src=m_doc.shapes[si];
    if(src.fill.type!=FillType::Bitmap || src.fill.image.isNull()) return;
    Fill::Adjust a;                              // effect presets
    switch(kind){ case 1: a.brightness=45; break; case 2: a.brightness=-45; break;
                  case 3: a.blur=6; break;       case 4: a.sharpen=70; break;
                  case 5: a.gray=true; break;    default: a.brightness=45; }
    // Bake from the DISPLAYED bitmap (already cropped + base-adjusted) so the overlay registers
    // pixel-for-pixel with the parallelogram it inherits, and keeps the base photo's own look.
    const QImage& base = src.fill.image;
    QImage baked = adjustPhoto(base, a.brightness,a.contrast,a.saturation, a.gray, false, a.redeye,
                               a.blur,a.sharpen,a.vignette, a.lens,a.hdr, a.blackPt,a.whitePt,a.gamma);
    if(baked.isNull()) return;
    pushUndo("Effect painter");
    Shape ov=src;                                // same transform + fill parallelogram → aligns exactly
    ov.fill.image=baked; ov.fill.origImage=baked; ov.fill.adjust=a;
    ov.regionPath=localRegion;                   // clip the adjusted copy to the painted area
    ov.shadow=Shadow(); ov.contour=Contour(); ov.bevel=Bevel(); ov.extrude=Extrude();  // don't duplicate live effects
    ov.name=src.name.isEmpty()? QStringLiteral("Effect") : src.name+" effect";
    m_doc.shapes.insert(si+1, ov);               // directly above the source photo
    m_sel={si+1}; emit documentChanged(); emit selectionChanged(); update();
}
void Canvas::applyToSelection(const std::function<void(Shape&)>& fn, const QString& label) {
    if (inGroup()) {                                  // edit the sub-selected object(s) inside the entered group
        Shape* c=enteredContainer(); if(!c || m_subSel.isEmpty()) return; pushUndo(label);
        for (int ci:m_subSel) if(ci>=0 && ci<c->children.size()) fn(c->children[ci]);
        emit documentChanged(); emit selectionChanged(); update(); return;
    }
    if (m_sel.isEmpty()) return;
    pushUndo(label);
    for (int i : m_sel) fn(m_doc.shapes[i]);
    syncLiveCopies();   // appearance edits to a linked master ripple to its live copies
    emit documentChanged(); emit selectionChanged(); update();
}

// Visit every colour of a shape (fill + gradient stops + mesh corners + contone tint + stroke), recursing into groups.
static void visitShapeColors(Shape& s, const std::function<QColor(const QColor&)>& fn) {
    if (s.fill.type != FillType::None) s.fill.color = fn(s.fill.color);
    s.fill.color2 = fn(s.fill.color2);
    for (GradStop& st : s.fill.stops) st.color = fn(st.color);
    for (QColor& c : s.fill.meshColors) c = fn(c);
    if (s.stroke.enabled) s.stroke.color = fn(s.stroke.color);
    for (Shape& ch : s.children) visitShapeColors(ch, fn);
}

// Selection-wide recolour: rotate hue (deg) and scale saturation / lightness of every colour.
void Canvas::recolorSelection(double hueDeg, double satScale, double lightScale) {
    const int dh = ((int(hueDeg)%360)+360)%360;
    auto xform = [=](const QColor& c)->QColor {
        int h,sv,l,a; c.getHsl(&h,&sv,&l,&a); if(h<0) h=0;   // achromatic → hue arbitrary (sat≈0 makes it moot)
        QColor o; o.setHsl((h+dh)%360, qBound(0,int(sv*satScale+0.5),255), qBound(0,int(l*lightScale+0.5),255), a); return o;
    };
    applyToSelection([&](Shape& s){ visitShapeColors(s, xform); }, "Recolour");
}

// Photo Cutout Studio (automatic background removal): flood-fill from the photo's borders keying out
// pixels within `tol` (RGB distance) of the mean border colour → background alpha 0, with a 1px soft
// matte (defringe) on the retained edge. Operates on each selected Bitmap-fill photo.
void Canvas::cutoutBackgroundSelection(int tol){
    applyToSelection([&](Shape& s){
        if(s.fill.type!=FillType::Bitmap || s.fill.image.isNull()) return;
        QImage img=s.fill.image.convertToFormat(QImage::Format_ARGB32);
        const int W=img.width(), H=img.height(); if(W<2||H<2) return;
        long sr=0,sg=0,sb=0; int n=0;                                   // mean border colour = the key
        for(int x=0;x<W;++x){ QRgb a=img.pixel(x,0),b=img.pixel(x,H-1); sr+=qRed(a)+qRed(b); sg+=qGreen(a)+qGreen(b); sb+=qBlue(a)+qBlue(b); n+=2; }
        for(int y=0;y<H;++y){ QRgb a=img.pixel(0,y),b=img.pixel(W-1,y); sr+=qRed(a)+qRed(b); sg+=qGreen(a)+qGreen(b); sb+=qBlue(a)+qBlue(b); n+=2; }
        const int mr=int(sr/n), mg=int(sg/n), mb=int(sb/n), t2=tol*tol;
        QVector<uchar> bg(W*H,0); QVector<int> stack; stack.reserve(W*H/4+16);
        auto keyed=[&](QRgb c){ int dr=qRed(c)-mr,dg=qGreen(c)-mg,db=qBlue(c)-mb; return dr*dr+dg*dg+db*db<=t2; };
        auto pushIf=[&](int x,int y){ int i=y*W+x; if(!bg[i] && keyed(img.pixel(x,y))){ bg[i]=1; stack.push_back(i); } };
        for(int x=0;x<W;++x){ pushIf(x,0); pushIf(x,H-1); }
        for(int y=0;y<H;++y){ pushIf(0,y); pushIf(W-1,y); }
        while(!stack.isEmpty()){ int i=stack.takeLast(), x=i%W, y=i/W;    // 4-connected flood fill
            if(x>0)pushIf(x-1,y); if(x<W-1)pushIf(x+1,y); if(y>0)pushIf(x,y-1); if(y<H-1)pushIf(x,y+1); }
        for(int y=0;y<H;++y){ QRgb* row=reinterpret_cast<QRgb*>(img.scanLine(y));
            for(int x=0;x<W;++x){ int i=y*W+x;
                if(bg[i]) row[x]=qRgba(qRed(row[x]),qGreen(row[x]),qBlue(row[x]),0);   // background → transparent
                else if((x>0&&bg[i-1])||(x<W-1&&bg[i+1])||(y>0&&bg[i-W])||(y<H-1&&bg[i+W]))
                    row[x]=qRgba(qRed(row[x]),qGreen(row[x]),qBlue(row[x]),150); } }   // soft matte edge (defringe)
        s.fill.image=img; s.fill.origImage=img; s.fill.adjust=Fill::Adjust();
    }, "Cutout background");
}

// ---- Guided graph-cut segmentation (interactive Cutout Studio) --------------
// Boykov–Jolly (2001) interactive foreground/background segmentation. The user paints
// "keep" and "remove" scribbles; those seed a colour model and hard graph constraints, and a
// min-cut of the pixel grid (via Dinic max-flow) separates object from background at the
// strongest nearby edges. Unlike the flood-fill keyer above this respects colour statistics
// AND image contours, so it handles textured/multi-colour backgrounds. Deterministic.
namespace {
// Dinic max-flow on a sparse graph. Symmetric n-links share residual capacity in both
// directions; terminal (source/sink) links are one-directional (reverse residual 0).
struct MaxFlow {
    struct E { int to; double cap; int rev; };
    std::vector<std::vector<E>> g;
    std::vector<int> level, iter;
    explicit MaxFlow(int n) : g(n), level(n), iter(n) {}
    void addEdge(int a, int b, double ca, double cb) {   // cb = reverse residual (n-link: cb=ca; t-link: cb=0)
        g[a].push_back({b, ca, (int)g[b].size()});
        g[b].push_back({a, cb, (int)g[a].size() - 1});
    }
    bool bfs(int s, int t) {
        std::fill(level.begin(), level.end(), -1);
        std::vector<int> q; q.reserve(level.size()); q.push_back(s); level[s] = 0;
        for (size_t h = 0; h < q.size(); ++h) { int v = q[h];
            for (const E& e : g[v]) if (e.cap > 1e-9 && level[e.to] < 0) { level[e.to] = level[v] + 1; q.push_back(e.to); } }
        return level[t] >= 0;
    }
    double dfs(int v, int t, double f) {
        if (v == t) return f;
        for (int& i = iter[v]; i < (int)g[v].size(); ++i) { E& e = g[v][i];
            if (e.cap > 1e-9 && level[v] < level[e.to]) {
                double d = dfs(e.to, t, std::min(f, e.cap));
                if (d > 1e-9) { e.cap -= d; g[e.to][e.rev].cap += d; return d; } } }
        return 0;
    }
    void maxflow(int s, int t) {
        while (bfs(s, t)) { std::fill(iter.begin(), iter.end(), 0);
            while (dfs(s, t, 1e30) > 1e-9) {} }
    }
    // After maxflow, level[] from a residual BFS marks the source side (reachable => foreground).
};
} // namespace

// Segment `srcIn` into keep/remove using scribbles in `seedIn` (ARGB overlay, same framing as the
// photo): a marked pixel (alpha>0) with green>red is a KEEP seed, else a REMOVE seed. Returns a
// Grayscale8 matte (255=keep, 0=remove) at the source resolution, or a null image if either class
// has no seed. Runs on a downsampled grid (≤180px) then upsamples the matte for a soft edge.
static QImage graphCutSegment(const QImage& srcIn, const QImage& seedIn) {
    if (srcIn.isNull() || seedIn.isNull()) return QImage();
    const int SW = srcIn.width(), SH = srcIn.height(); if (SW < 2 || SH < 2) return QImage();
    const double sc = qMin(1.0, 180.0 / qMax(SW, SH));
    const int W = qMax(2, int(SW * sc)), H = qMax(2, int(SH * sc));
    QImage img  = srcIn.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
    QImage seed = seedIn.scaled(W, H, Qt::IgnoreAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_ARGB32);
    const int N = W * H;
    std::vector<int> label(N, 0);                    // 0 none, 1 keep-seed, 2 remove-seed
    std::vector<double> histFG(4096, 0.0), histBG(4096, 0.0);
    double fgTot = 0, bgTot = 0; int fgN = 0, bgN = 0;
    auto binOf = [](QRgb c) { return ((qRed(c) >> 4) << 8) | ((qGreen(c) >> 4) << 4) | (qBlue(c) >> 4); };
    for (int y = 0; y < H; ++y) { const QRgb* srow = reinterpret_cast<const QRgb*>(seed.constScanLine(y));
        const QRgb* irow = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < W; ++x) { QRgb sp = srow[x]; if (qAlpha(sp) < 16) continue;
            int b = binOf(irow[x]);
            if (qGreen(sp) > qRed(sp)) { label[y * W + x] = 1; histFG[b] += 1.0; ++fgN; }
            else                       { label[y * W + x] = 2; histBG[b] += 1.0; ++bgN; } } }
    if (fgN == 0 || bgN == 0) return QImage();
    for (double v : histFG) fgTot += v; for (double v : histBG) bgTot += v;
    // Auto contrast scale for n-links: sigma^2 = mean squared colour difference over adjacent pixels.
    double diffSum = 0; int diffN = 0;
    auto d2 = [&](QRgb a, QRgb b) { int dr = qRed(a)-qRed(b), dg = qGreen(a)-qGreen(b), db = qBlue(a)-qBlue(b); return double(dr*dr + dg*dg + db*db); };
    for (int y = 0; y < H; ++y) { const QRgb* r0 = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        const QRgb* r1 = (y + 1 < H) ? reinterpret_cast<const QRgb*>(img.constScanLine(y + 1)) : nullptr;
        for (int x = 0; x < W; ++x) { if (x + 1 < W) { diffSum += d2(r0[x], r0[x+1]); ++diffN; }
            if (r1) { diffSum += d2(r0[x], r1[x]); ++diffN; } } }
    const double sigma2 = qMax(1.0, diffN ? diffSum / diffN : 1.0);
    const double LAMBDA = 1.2, GAMMA = 12.0, K = 1e6;
    MaxFlow mf(N + 2); const int S = N, T = N + 1;
    for (int y = 0; y < H; ++y) { const QRgb* irow = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < W; ++x) { int p = y * W + x;
            if (label[p] == 1)      { mf.addEdge(S, p, K, 0); }
            else if (label[p] == 2) { mf.addEdge(p, T, K, 0); }
            else { int b = binOf(irow[x]);
                double pFG = (histFG[b] + 1.0) / (fgTot + 4096.0), pBG = (histBG[b] + 1.0) / (bgTot + 4096.0);
                double Rfg = -std::log(pFG), Rbg = -std::log(pBG);
                mf.addEdge(S, p, LAMBDA * Rbg, 0);   // cut S→p ⇒ p on sink side (remove); cheap when p looks like bg
                mf.addEdge(p, T, LAMBDA * Rfg, 0); } } }
    for (int y = 0; y < H; ++y) { const QRgb* r0 = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        const QRgb* r1 = (y + 1 < H) ? reinterpret_cast<const QRgb*>(img.constScanLine(y + 1)) : nullptr;
        for (int x = 0; x < W; ++x) { int p = y * W + x;
            if (x + 1 < W) { double w = GAMMA * std::exp(-d2(r0[x], r0[x+1]) / (2.0 * sigma2)) + 0.02; mf.addEdge(p, p + 1, w, w); }
            if (r1)        { double w = GAMMA * std::exp(-d2(r0[x], r1[x]) / (2.0 * sigma2)) + 0.02; mf.addEdge(p, p + W, w, w); } } }
    mf.maxflow(S, T);
    mf.bfs(S, T);   // residual reachability from source ⇒ foreground side
    QImage small(W, H, QImage::Format_Grayscale8);
    for (int y = 0; y < H; ++y) { uchar* row = small.scanLine(y);
        for (int x = 0; x < W; ++x) row[x] = (mf.level[y * W + x] >= 0) ? 255 : 0; }
    return small.scaled(SW, SH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);   // upsample → soft matte
}

// Apply a Cutout Studio result: turn the graph-cut matte into the photo's transparency mask
// (geom-4 luminance mask — the same path the Paint-Mask painter and XAR use), so it renders and
// persists with no new code and stays fully re-editable / removable.
bool Canvas::cutoutStudioApply(int si, const QImage& seed) {
    if (si < 0 || si >= m_doc.shapes.size()) return false;
    Shape& s = m_doc.shapes[si];
    if (s.fill.type != FillType::Bitmap || s.fill.image.isNull()) { emit statusMessage("Cutout Studio needs a photo (bitmap object) selected."); return false; }
    QImage mask = graphCutSegment(s.fill.image, seed);
    if (mask.isNull()) { emit statusMessage("Cutout Studio: paint at least one KEEP stroke and one REMOVE stroke, then apply."); return false; }
    pushUndo("Cutout Studio");
    s.transp.maskBmp = mask.convertToFormat(QImage::Format_ARGB32);
    s.transp.geom = 4; if (s.transp.type <= 0) s.transp.type = 1; s.transp.t0 = 1.0; s.transp.t1 = 0.0;
    // The matte is in fill.image pixel space, so the mask parallelogram must match the bitmap fill's own
    // (p0/p1/p2), NOT the axis-aligned localBounds — otherwise the cutout misaligns once the fill is moved/
    // scaled/rotated with the Fill tool. (For a freshly imported photo these are equal → selftest identical.)
    s.transp.p0 = s.fill.p0; s.transp.p1 = s.fill.p1; s.transp.p2 = s.fill.p2;
    emit documentChanged(); emit selectionChanged(); update();
    emit statusMessage("Cutout applied — background hidden via a re-editable transparency mask (Transparency tool → Paint Mask to refine).");
    return true;
}

// Photo perspective / keystone correction: warp the selected photo's bitmap by a projective transform to
// counteract converging verticals/horizontals. kv = vertical keystone (top-vs-bottom width), kh = horizontal
// keystone (left-vs-right height), each roughly [-0.6,+0.6]. Bakes into fill.image/origImage (destructive,
// like the flood-fill cutout); kv==0 && kh==0 is a no-op.
bool Canvas::correctPerspective(int si, double kv, double kh){
    if(si<0 || si>=m_doc.shapes.size()) return false;
    Shape& s=m_doc.shapes[si];
    if(s.fill.type!=FillType::Bitmap || s.fill.image.isNull()){ emit statusMessage("Perspective correction needs a photo (bitmap object) selected."); return false; }
    if(qAbs(kv)<1e-4 && qAbs(kh)<1e-4) return false;
    // Warp the CURRENTLY-VISIBLE (adjusted) image, not the pristine source — matches the dialog preview and
    // preserves any non-destructive photo adjustment (baked into the visible image), mirroring straighten().
    QImage src=(s.fill.image.isNull()? s.fill.origImage : s.fill.image).convertToFormat(QImage::Format_ARGB32);
    const int W=src.width(), H=src.height(); if(W<2||H<2) return false;
    kv=qBound(-0.85,kv,0.85); kh=qBound(-0.85,kh,0.85);
    // Keep the destination quad strictly convex: |kv|+|kh|>=1 makes two corners collinear (quadToQuad fails →
    // silent no-op) or concave (folded/garbled bake). Proportionally shrink both so the sum stays clear of 1.
    if(double ks=qAbs(kv)+qAbs(kh); ks>0.85){ kv*=0.85/ks; kh*=0.85/ks; }
    const double hw=W/2.0, hh=H/2.0, cx=hw, cy=hh;
    QPolygonF srcQ; srcQ<<QPointF(0,0)<<QPointF(W,0)<<QPointF(W,H)<<QPointF(0,H);
    QPolygonF dstQ;                                       // kv>0 spreads the TOP wider (un-converges verticals); kh>0 spreads the LEFT taller
    dstQ<<QPointF(cx-hw*(1+kv), cy-hh*(1+kh))            // top-left
        <<QPointF(cx+hw*(1+kv), cy-hh*(1-kh))            // top-right
        <<QPointF(cx+hw*(1-kv), cy+hh*(1-kh))            // bottom-right
        <<QPointF(cx-hw*(1-kv), cy+hh*(1+kh));           // bottom-left
    QTransform t; if(!QTransform::quadToQuad(srcQ, dstQ, t)) return false;
    QImage out(W,H,QImage::Format_ARGB32); out.fill(Qt::transparent);
    { QPainter g(&out); g.setRenderHints(QPainter::SmoothPixmapTransform|QPainter::Antialiasing); g.setTransform(t); g.drawImage(0,0,src); }
    pushUndo("Correct perspective");
    s.fill.image=out; s.fill.origImage=out; s.fill.adjust=Fill::Adjust();
    emit documentChanged(); emit selectionChanged(); update();
    emit statusMessage("Perspective corrected — the photo's converging edges were straightened (edges may be transparent).");
    return true;
}

// Selection-wide colour replace: every colour within `tol` (RGB distance) of `from` becomes `to` (alpha preserved).
void Canvas::replaceColorInSelection(const QColor& from, const QColor& to, int tol) {
    const int t2 = tol*tol;
    auto xform = [=](const QColor& c)->QColor {
        const int dr=c.red()-from.red(), dg=c.green()-from.green(), db=c.blue()-from.blue();
        if (dr*dr+dg*dg+db*db <= t2){ QColor o=to; o.setAlpha(c.alpha()); return o; } return c;
    };
    applyToSelection([&](Shape& s){ visitShapeColors(s, xform); }, "Replace colour");
}

void Canvas::syncLiveCopies() {
    QHash<int,int> master;                       // linkId -> master (lowest) index
    for (int i=0;i<m_doc.shapes.size();++i){ int id=m_doc.shapes[i].linkId; if(id>0 && !master.contains(id)) master[id]=i; }
    for (int i=0;i<m_doc.shapes.size();++i){ int id=m_doc.shapes[i].linkId; if(id<=0) continue; int m=master.value(id,-1); if(m<0||m==i) continue;
        QTransform keepXf=m_doc.shapes[i].transform; int keepLayer=m_doc.shapes[i].layer, keepSoft=m_doc.shapes[i].softGroup;
        Shape copy=m_doc.shapes[m]; copy.transform=keepXf; copy.layer=keepLayer; copy.linkId=id; copy.softGroup=keepSoft; copy.locked=false;
        m_doc.shapes[i]=copy; }
}

void Canvas::makeLiveCopy() {
    if (m_sel.isEmpty()) return; pushUndo("Live copy");
    int id=0; for(auto& s:m_doc.shapes) id=qMax(id,s.linkId); id++;
    QVector<int> nsel;
    for (int i:m_sel){ m_doc.shapes[i].linkId=id; Shape c=m_doc.shapes[i]; c.transform=c.transform*QTransform::fromTranslate(24,24); nsel.push_back(m_doc.addShape(c)); }
    m_sel=nsel; emit documentChanged(); emit selectionChanged(); update();
}

void Canvas::makeSoftGroup() {
    if (m_sel.size()<2) return; pushUndo("Soft group");
    int id=0; for(auto& s:m_doc.shapes) id=qMax(id,s.softGroup); id++;
    for(int i:m_sel) m_doc.shapes[i].softGroup=id; emit documentChanged(); update();
}
void Canvas::ungroupSoftGroup() {
    if (m_sel.isEmpty()) return; pushUndo("Ungroup soft");
    for(int i:m_sel) m_doc.shapes[i].softGroup=0; emit documentChanged(); update();
}

void Canvas::addShapes(const QVector<Shape>& s, const QString& label) {
    if (s.isEmpty()) return;
    pushUndo(label);
    QVector<int> nsel;
    for (Shape sh : s) { sh.layer=m_activeLayer; nsel.push_back(m_doc.addShape(sh)); }
    // Don't auto-select thousands of freshly-imported objects: the per-selection UI (InfoBar bounds,
    // Properties panel, gallery highlighting) doesn't scale to that many, so a big import (e.g. a 9k-object
    // .cdr) would otherwise freeze on the selectionChanged refresh. A huge selection isn't useful anyway.
    m_sel = (nsel.size() > 2000) ? QVector<int>() : nsel;
    emit documentChanged(); emit selectionChanged(); update();
}

void Canvas::lockSelection(){ if(m_sel.isEmpty())return; pushUndo("Lock"); for(int i:m_sel) m_doc.shapes[i].locked=true; emit documentChanged(); update(); }
void Canvas::unlockSelection(){ if(m_sel.isEmpty())return; pushUndo("Unlock"); for(int i:m_sel) m_doc.shapes[i].locked=false; emit documentChanged(); update(); }
void Canvas::addGuide(bool horizontal, double pos){ pushUndo("Add guide"); (horizontal?m_guideHpos:m_guideVpos).push_back(pos); update(); }

// ---- layers ----------------------------------------------------------------
void Canvas::addLayer(){ pushUndo("New layer"); Layer l; l.name=QString("Layer %1").arg(m_doc.layers.size()+1); m_doc.layers.push_back(l); m_activeLayer=m_doc.layers.size()-1; emit documentChanged(); update(); }
void Canvas::removeLayer(int i){ if(i<0||i>=m_doc.layers.size()||m_doc.layers.size()<=1) return; invalidateMould(); resetTextEdit(); pushUndo("Delete layer");
    // drop shapes on this layer; renumber shapes above it down by one
    for(int k=m_doc.shapes.size()-1;k>=0;--k){ if(m_doc.shapes[k].layer==i) m_doc.shapes.remove(k); else if(m_doc.shapes[k].layer>i) m_doc.shapes[k].layer--; }
    m_doc.layers.remove(i); m_activeLayer=qBound(0,m_activeLayer,m_doc.layers.size()-1); m_sel.clear(); emit documentChanged(); emit selectionChanged(); update(); }
void Canvas::renameLayer(int i,const QString& n){ if(i<0||i>=m_doc.layers.size())return; pushUndo("Rename layer"); m_doc.layers[i].name=n; emit documentChanged(); }
void Canvas::setLayerVisible(int i,bool v){ if(i<0||i>=m_doc.layers.size())return; pushUndo("Layer visibility"); m_doc.layers[i].visible=v; emit documentChanged(); update(); }
void Canvas::setLayerLocked(int i,bool l){ if(i<0||i>=m_doc.layers.size())return; pushUndo("Layer lock"); m_doc.layers[i].locked=l; emit documentChanged(); update(); }
void Canvas::setLayerMaster(int i,bool m){ if(i<0||i>=m_doc.layers.size())return; pushUndo("Master layer"); m_doc.layers[i].master=m; emit documentChanged(); update(); }
void Canvas::setLayerPrintable(int i,bool p){ if(i<0||i>=m_doc.layers.size())return; pushUndo("Non-printing layer"); m_doc.layers[i].printable=p; emit documentChanged(); update(); }
bool Canvas::layerPrintable(int i) const { return i<0 || i>=m_doc.layers.size() || m_doc.layers[i].printable; }
// Optimize Photos: downsample each placed photo's embedded bitmap to the pixels actually needed at its
// on-canvas display size (with a 1.5× headroom for print/retina) — Xara's "Optimize Photos". Returns the
// number optimised; reports the megapixels saved. Operates on the selection, or the whole doc if none.
int Canvas::optimizePhotos(double dpiHeadroom){
    QVector<int> targets = m_sel.isEmpty() ? QVector<int>() : m_sel;
    if(targets.isEmpty()) for(int i=0;i<m_doc.shapes.size();++i) targets.push_back(i);
    int n=0; qint64 pxBefore=0, pxAfter=0;
    for(int i: targets){ if(i<0||i>=m_doc.shapes.size()) continue; Shape& s=m_doc.shapes[i];
        if(s.fill.type!=FillType::Bitmap || s.fill.image.isNull()) continue;
        const QSizeF disp=s.docBounds().size();
        const int capW=qMax(1,int(std::ceil(disp.width() *qMax(1.0,dpiHeadroom))));
        const int capH=qMax(1,int(std::ceil(disp.height()*qMax(1.0,dpiHeadroom))));
        if(s.fill.image.width()<=capW && s.fill.image.height()<=capH) continue;   // already at/under the needed size
        if(n==0) pushUndo("Optimize photos");
        pxBefore += qint64(s.fill.image.width())*s.fill.image.height();
        QImage down=s.fill.image.scaled(capW,capH,Qt::KeepAspectRatio,Qt::SmoothTransformation);
        s.fill.image=down; if(!s.fill.origImage.isNull()) s.fill.origImage=down;   // trim the stored source too
        pxAfter += qint64(down.width())*down.height(); ++n;
    }
    if(n>0){ emit documentChanged(); update();
        emit statusMessage(QString("Optimized %1 photo%2 — %3 → %4 MP embedded (%5% smaller).")
            .arg(n).arg(n==1?"":"s").arg(pxBefore/1e6,0,'f',1).arg(pxAfter/1e6,0,'f',1)
            .arg(pxBefore>0?int(100.0*(pxBefore-pxAfter)/pxBefore):0)); }
    else emit statusMessage("No photos needed optimizing (all are already at their display resolution).");
    return n;
}
void Canvas::moveSelectionToLayer(int i){ if(m_sel.isEmpty()||i<0||i>=m_doc.layers.size())return; pushUndo("Move to layer"); for(int s:m_sel) m_doc.shapes[s].layer=i; emit documentChanged(); update(); }
void Canvas::moveLayer(int i,int delta){ int j=i+delta; if(i<0||i>=m_doc.layers.size()||j<0||j>=m_doc.layers.size())return; pushUndo("Reorder layer");
    std::swap(m_doc.layers[i],m_doc.layers[j]); for(auto& s:m_doc.shapes){ if(s.layer==i)s.layer=j; else if(s.layer==j)s.layer=i; } emit documentChanged(); update(); }
void Canvas::soloLayer(int i){ if(i<0||i>=m_doc.layers.size())return; pushUndo("Solo layer");
    bool alreadySolo=true; for(int k=0;k<m_doc.layers.size();++k) if(m_doc.layers[k].visible != (k==i)){ alreadySolo=false; break; }
    for(int k=0;k<m_doc.layers.size();++k) m_doc.layers[k].visible = alreadySolo ? true : (k==i); emit documentChanged(); update(); }
void Canvas::setShapeVisible(int idx,bool v){ if(idx<0||idx>=m_doc.shapes.size())return; pushUndo("Object visibility"); m_doc.shapes[idx].visible=v; emit documentChanged(); update(); }
void Canvas::setShapeLocked(int idx,bool l){ if(idx<0||idx>=m_doc.shapes.size())return; pushUndo("Object lock"); m_doc.shapes[idx].locked=l; emit documentChanged(); update(); }
void Canvas::renameShape(int idx,const QString& n){ if(idx<0||idx>=m_doc.shapes.size())return; if(m_doc.shapes[idx].name==n)return; pushUndo("Rename object"); m_doc.shapes[idx].name=n; emit documentChanged(); }
void Canvas::setObjectOrder(const QVector<int>& order,const QVector<int>& newLayer){
    const int n=m_doc.shapes.size(); const int nL=m_doc.layers.size();
    if(order.size()!=n || newLayer.size()!=n || nL<1) return;      // malformed → ignore (never corrupt)
    QVector<char> seen(n,0); for(int k=0;k<n;++k){ int o=order[k]; if(o<0||o>=n||seen[o]){ return; } seen[o]=1; }  // must be a permutation
    // True no-op detection: nothing moved iff every layer's WITHIN-LAYER sequence of shapes is
    // unchanged (paint order only depends on layer membership + within-layer order, never on the
    // flat array's cross-layer interleaving). This makes a drop-in-place — even on a doc whose
    // shapes array is interleaved across layers — leave the model, undo stack and file bytes intact.
    QVector<QVector<int>> curSeq(nL), newSeq(nL);
    for(int i=0;i<n;++i) curSeq[qBound(0,m_doc.shapes[i].layer,nL-1)].push_back(i);
    for(int k=0;k<n;++k) newSeq[qBound(0,newLayer[k],nL-1)].push_back(order[k]);
    if(curSeq==newSeq) return;
    resetGroupEntry();   // this reorders the top-level array; drop any stale group-descent (see review H-16 finding)
    pushUndo("Reorder objects");
    QVector<Shape> rebuilt; rebuilt.reserve(n);
    QVector<int> remap(n);   // old index -> new index
    for(int k=0;k<n;++k){ Shape s=m_doc.shapes[order[k]]; s.layer=qBound(0,newLayer[k],nL-1); remap[order[k]]=k; rebuilt.push_back(s); }
    m_doc.shapes=rebuilt;
    // Remap the selection AND every absolute cross-reference index (linked-text overflow targets and
    // inline-graphic anchors point at Document.shapes slots — a bare permutation would leave them
    // pointing at whatever now occupies the old slot, silently breaking chains/anchors).
    for(int& si:m_sel) si=remap.value(si,si);
    if(m_textEdit>=0) m_textEdit=remap.value(m_textEdit,-1);   // the caret follows its shape to its new index
    for(Shape& s: m_doc.shapes){
        if(s.nextFrame>=0)       s.nextFrame      = remap.value(s.nextFrame,s.nextFrame);
        if(s.anchorTextIndex>=0) s.anchorTextIndex= remap.value(s.anchorTextIndex,s.anchorTextIndex);
    }
    invalidateMould();   // indices moved — any cached mould reference is now stale
    syncTextEditToDoc();
    emit documentChanged(); emit selectionChanged(); update(); }

void Canvas::deleteSelection(){
    if(inGroup()){ Shape* c=enteredContainer(); if(!c||m_subSel.isEmpty())return; pushUndo("Delete in group");
        QVector<int> s=m_subSel; std::sort(s.begin(),s.end(),std::greater<int>());
        for(int ci:s) if(ci>=0&&ci<c->children.size()) c->children.removeAt(ci);
        m_subSel.clear(); emit documentChanged(); emit selectionChanged(); update(); return; }
    if(m_sel.isEmpty())return; if(m_cropMode) cancelCropMode(); invalidateMould(); resetTextEdit(); pushUndo("Delete");
    std::sort(m_sel.begin(),m_sel.end(),std::greater<int>());
    for(int i:m_sel) m_doc.shapes.removeAt(i);
    m_sel.clear(); emit documentChanged(); emit selectionChanged(); update(); }

void Canvas::duplicateSelection(bool inPlace){ if(m_sel.isEmpty())return; resetGroupEntry(); pushUndo(inPlace?"Clone":"Duplicate");
    QVector<int> ns; for(int i:m_sel){ Shape s=m_doc.shapes[i]; if(!inPlace) s.transform.translate(14,14); m_doc.shapes.push_back(s); ns.push_back(m_doc.shapes.size()-1);}
    m_sel=ns; emit documentChanged(); emit selectionChanged(); update(); }
void Canvas::radialArray(int count, double totalDeg){
    if(m_sel.isEmpty() || count<2) return; resetGroupEntry(); pushUndo("Radial array");
    const QPointF c=selBoundsDoc().center();
    QVector<Shape> src; for(int i:m_sel) src.push_back(m_doc.shapes[i]);
    QVector<int> ns=m_sel;
    for(int k=1;k<count;++k){
        QTransform rot; rot.translate(c.x(),c.y()); rot.rotate(totalDeg*k/count); rot.translate(-c.x(),-c.y());
        for(const Shape& s0: src){ Shape s=s0; s.transform=s.transform*rot; m_doc.shapes.push_back(s); ns.push_back(m_doc.shapes.size()-1); } }
    m_sel=ns; emit documentChanged(); emit selectionChanged(); update();
}

void Canvas::copySelection(){ if(m_sel.isEmpty())return; QJsonArray a;
    QVector<int> s=m_sel; std::sort(s.begin(),s.end()); for(int i:s) a.append(m_doc.shapes[i].toJson());
    m_clip=QJsonDocument(a).toJson(QJsonDocument::Compact);
    // Also render a bitmap onto the OS clipboard, so the artwork can be pasted into other apps (browser/Word/…).
    QRectF b=selBoundsDoc();
    if(!b.isNull() && b.width()<5000 && b.height()<5000){
        QImage img(QSize(int(b.width()*2),int(b.height()*2)), QImage::Format_ARGB32_Premultiplied);
        if(!img.isNull() && !img.size().isEmpty()){ img.fill(Qt::transparent);
            Document tmp; tmp.pageSize=m_doc.pageSize; for(int i:s) tmp.shapes.push_back(m_doc.shapes[i]);
            QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
            g.scale(2,2); g.translate(-b.topLeft()); paintDocument(g, tmp); g.end();
            QGuiApplication::clipboard()->setImage(img); } } }
// Paste a bitmap sitting on the OS clipboard (e.g. a screenshot) as a new photo object. Final fallback in
// the Paste chain (after the internal shape clipboard and rich text).
// Create a new photo (Bitmap-fill Rect) from a raw image, centred at centerDoc, and select it.
// Single source of truth for the bitmap-fill parallelogram convention — see render.cpp bitmapTexXf:
// image top-left maps to p2, bottom-left to p0, so p0=bottom-left / p1=bottom-right / p2=top-left is
// upright. (p0=(0,0)/p2=(0,H) would negate the Y step and flip the image vertically.)
int Canvas::placeBitmap(const QImage& src, const QPointF& centerDoc, const char* undoLabel){
    if(src.isNull()) return -1;
    QImage img = (src.format()==QImage::Format_ARGB32) ? src : src.convertToFormat(QImage::Format_ARGB32);
    pushUndo(undoLabel);
    Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,img.width(),img.height());
    s.fill.type=FillType::Bitmap; s.fill.image=img; s.fill.origImage=img;
    s.fill.p0=QPointF(0,img.height()); s.fill.p1=QPointF(img.width(),img.height()); s.fill.p2=QPointF(0,0);
    s.stroke.enabled=false; s.name="Pasted image";
    s.transform=QTransform::fromTranslate(centerDoc.x()-img.width()/2.0, centerDoc.y()-img.height()/2.0);
    m_doc.shapes.push_back(s); const int idx=m_doc.shapes.size()-1; m_sel={idx};
    emit documentChanged(); emit selectionChanged(); update(); return idx; }
bool Canvas::pasteBitmapFromClipboard(){
    QImage img=QGuiApplication::clipboard()->image();
    if(img.isNull()) return false;
    return placeBitmap(img, toDoc(QPointF(width()/2.0,height()/2.0)), "Paste bitmap") >= 0; }
void Canvas::cutSelection(){ if(m_sel.isEmpty())return; copySelection(); deleteSelection(); }
bool Canvas::pasteClipboard(){ if(m_clip.isEmpty())return false; QJsonArray a=QJsonDocument::fromJson(m_clip).array(); if(a.isEmpty())return false;
    pushUndo("Paste"); QVector<int> ns;
    for(auto v:a){ Shape s=Shape::fromJson(v.toObject()); s.transform.translate(16,16); m_doc.shapes.push_back(s); ns.push_back(m_doc.shapes.size()-1); }
    m_sel=ns; emit documentChanged(); emit selectionChanged(); update(); return true; }
// Paste in place: paste at the copied objects' ORIGINAL coordinates (no offset) — for moving art between
// pages/layers without drift.
bool Canvas::pasteInPlace(){ if(m_clip.isEmpty())return false; QJsonArray a=QJsonDocument::fromJson(m_clip).array(); if(a.isEmpty())return false;
    pushUndo("Paste in place"); QVector<int> ns;
    for(auto v:a){ Shape s=Shape::fromJson(v.toObject()); m_doc.shapes.push_back(s); ns.push_back(m_doc.shapes.size()-1); }
    m_sel=ns; emit documentChanged(); emit selectionChanged(); update(); return true; }
// Paste attributes: apply the copied object's appearance (fill colours/type, stroke, opacity, blend, and
// the live effects) onto the selection, KEEPING each target's own gradient geometry.
bool Canvas::pasteAttributes(){ if(m_clip.isEmpty()||m_sel.isEmpty())return false; QJsonArray a=QJsonDocument::fromJson(m_clip).array(); if(a.isEmpty())return false;
    const Shape src=Shape::fromJson(a.first().toObject());
    applyToSelection([&](Shape& s){ const QPointF p0=s.fill.p0,p1=s.fill.p1,p2=s.fill.p2;
        s.fill=src.fill; s.fill.p0=p0; s.fill.p1=p1; s.fill.p2=p2;
        s.stroke=src.stroke; s.opacity=src.opacity; s.blend=src.blend;
        s.shadow=src.shadow; s.contour=src.contour; s.bevel=src.bevel; s.feather=src.feather; }, "Paste attributes");
    return true; }

void Canvas::raiseSelection(){ if(m_sel.isEmpty())return; resetGroupEntry(); if(m_cropMode) cancelCropMode(); pushUndo("Raise"); std::sort(m_sel.begin(),m_sel.end(),std::greater<int>());
    for(int& i:m_sel){ if(i<m_doc.shapes.size()-1){ m_doc.shapes.swapItemsAt(i,i+1); i++; } } emit documentChanged(); update(); }
void Canvas::lowerSelection(){ if(m_sel.isEmpty())return; resetGroupEntry(); if(m_cropMode) cancelCropMode(); pushUndo("Lower"); std::sort(m_sel.begin(),m_sel.end());
    for(int& i:m_sel){ if(i>0){ m_doc.shapes.swapItemsAt(i,i-1); i--; } } emit documentChanged(); update(); }
void Canvas::toFront(){ if(m_sel.isEmpty())return; resetGroupEntry(); if(m_cropMode) cancelCropMode(); pushUndo("To Front"); QVector<Shape> mv; std::sort(m_sel.begin(),m_sel.end(),std::greater<int>());
    for(int i:m_sel){ mv.push_front(m_doc.shapes[i]); m_doc.shapes.removeAt(i);} QVector<int> ns; for(auto&s:mv){m_doc.shapes.push_back(s); ns.push_back(m_doc.shapes.size()-1);} m_sel=ns; emit documentChanged(); emit selectionChanged(); update(); }
void Canvas::toBack(){ if(m_sel.isEmpty())return; resetGroupEntry(); if(m_cropMode) cancelCropMode(); pushUndo("To Back"); QVector<Shape> mv; std::sort(m_sel.begin(),m_sel.end(),std::greater<int>());
    for(int i:m_sel){ mv.push_front(m_doc.shapes[i]); m_doc.shapes.removeAt(i);} QVector<int> ns; for(int k=mv.size()-1;k>=0;--k){m_doc.shapes.push_front(mv[k]);} for(int i=0;i<mv.size();++i) ns.push_back(i); m_sel=ns; emit documentChanged(); emit selectionChanged(); update(); }

// Map a doc-space point to a pixel coordinate of shape si's bitmap fill image.
// Mirrors render.cpp bitmapTexXf: image(0,0)->p2, image(W,0)->p2+(p1-p0), image(0,H)->p0.
bool Canvas::docToImagePixel(int si, const QPointF& docPt, QPointF& outImg) const {
    if (si<0 || si>=m_doc.shapes.size()) return false;
    const Shape& s=m_doc.shapes[si];
    if (s.fill.type!=FillType::Bitmap || s.fill.image.isNull()) return false;
    const double W=qMax(1,s.fill.image.width()), H=qMax(1,s.fill.image.height());
    const QPointF ax=(s.fill.p1-s.fill.p0)/W, ay=(s.fill.p0-s.fill.p2)/H;
    QTransform tex(ax.x(),ax.y(),ay.x(),ay.y(),s.fill.p2.x(),s.fill.p2.y());
    bool ok=false; QTransform inv=tex.inverted(&ok); if(!ok) return false;
    outImg = inv.map(s.transform.inverted().map(docPt));
    return true;
}

// Soft eraser: paint a feathered alpha hole into a photo's bitmap at a doc-space point. The nib
// radius (doc units) is mapped into image pixels; alpha is multiplicatively reduced with a smooth
// centre→rim falloff so repeated dabs deepen the erase and edges stay soft.
void Canvas::softEraseAt(int si, const QPointF& docPt){
    if(si<0 || si>=m_doc.shapes.size()) return;
    Shape& s=m_doc.shapes[si];
    if(s.fill.type!=FillType::Bitmap || s.fill.image.isNull()) return;
    QPointF c, e; if(!docToImagePixel(si,docPt,c)) return; docToImagePixel(si,docPt+QPointF(m_eraserNib,0),e);
    const double R=qMax(1.0, QLineF(c,e).length());
    QImage& img=s.fill.image;
    if(img.format()!=QImage::Format_ARGB32) img=img.convertToFormat(QImage::Format_ARGB32);
    if(img.isNull()) return;
    const int W=img.width(), H=img.height();
    const int x0=qMax(0,int(c.x()-R)), x1=qMin(W-1,int(c.x()+R)), y0=qMax(0,int(c.y()-R)), y1=qMin(H-1,int(c.y()+R));
    for(int y=y0;y<=y1;++y){ QRgb* ln=reinterpret_cast<QRgb*>(img.scanLine(y));
        for(int x=x0;x<=x1;++x){ double t=QLineF(c,QPointF(x,y)).length()/R; if(t>=1.0) continue;
            const double k=(1.0-t*t)*(1.0-t*t);                       // erase strength: strong centre, soft rim
            QRgb p=ln[x]; int a=qAlpha(p); int na=qBound(0,int(a*(1.0-k)+0.5),255);
            ln[x]=qRgba(qRed(p),qGreen(p),qBlue(p),na); } }
    // The erased pixels become the new non-destructive source; flatten the adjust stack (the current
    // look is already baked into `img`) so a later re-bake doesn't double-apply brightness/levels/etc.
    s.fill.origImage=img; s.fill.adjust=Fill::Adjust();
}

// Paint one soft round dab: copy from `srcSnap` (pre-stroke snapshot) at the source offset into
// s.fill.image. Heal (m_cloneMode==1) shifts the source patch's mean colour to the destination's.
// Local red-eye removal: de-red the pixels inside the brush circle (same test as the global Enhance
// red-eye, but scoped to the dab). Radius = the clone nib, in image pixels.
void Canvas::redeyeDab(Shape& s, QPointF destImg) {
    QImage& img=s.fill.image; if(img.isNull()) return;
    if(img.format()!=QImage::Format_ARGB32) img=img.convertToFormat(QImage::Format_ARGB32);
    const int W=img.width(), H=img.height(); const double R=qMax(1.0,m_cloneNib);
    for(int y=qFloor(destImg.y()-R);y<=qCeil(destImg.y()+R);++y){ if(y<0||y>=H)continue;
        QRgb* row=(QRgb*)img.scanLine(y);
        for(int x=qFloor(destImg.x()-R);x<=qCeil(destImg.x()+R);++x){ if(x<0||x>=W)continue;
            const double dx=x-destImg.x(), dy=y-destImg.y(); if(dx*dx+dy*dy>R*R)continue;
            const QRgb p=row[x]; int rr=qRed(p),gg=qGreen(p),bb=qBlue(p); const int mx=qMax(gg,bb);
            if(rr>mx*1.4 && rr>60){ row[x]=qRgba(int(mx*0.8),int(mx*0.85),int(mx*0.85),qAlpha(p)); } } }
    s.fill.origImage=QImage();   // permanent pixels — drop the stale photo-adjust source
}
void Canvas::cloneDab(Shape& s, const QImage& srcSnap, QPointF destImg, QPointF srcImg) {
    QImage& img=s.fill.image; if(img.isNull() || srcSnap.isNull()) return;
    if(img.format()!=QImage::Format_ARGB32) img=img.convertToFormat(QImage::Format_ARGB32);
    if(img.isNull()) return;
    const int W=img.width(), H=img.height(), SW=srcSnap.width(), SH=srcSnap.height();
    const double R=qMax(1.0,m_cloneNib), h=m_cloneHardness;
    auto falloff=[&](double t)->double{ if(t>=1) return 0; if(t<=h) return 1; double u=(t-h)/qMax(1e-4,1.0-h); return 1.0-(u*u*(3-2*u)); };
    auto srcAt=[&](double x,double y){ int sx=qBound(0,qRound(x-destImg.x()+srcImg.x()),SW-1), sy=qBound(0,qRound(y-destImg.y()+srcImg.y()),SH-1); return srcSnap.pixelColor(sx,sy); };  // clamp source to edge (no holes)
    // Heal: mean colour difference (pre-stroke dest - source) over the dab, to colour-match the patch.
    double mdr=0,mdg=0,mdb=0; int mc=0;
    if(m_cloneMode==1){ for(int y=qFloor(destImg.y()-R);y<=qCeil(destImg.y()+R);++y){ if(y<0||y>=H)continue;
        for(int x=qFloor(destImg.x()-R);x<=qCeil(destImg.x()+R);++x){ if(x<0||x>=W)continue;
            if(std::hypot(x-destImg.x(),y-destImg.y())>R) continue;
            QColor dc=srcSnap.pixelColor(qBound(0,x,SW-1),qBound(0,y,SH-1)), sc=srcAt(x,y);   // dc = pre-stroke destination pixel
            mdr+=dc.red()-sc.red(); mdg+=dc.green()-sc.green(); mdb+=dc.blue()-sc.blue(); ++mc; } } }
    if(mc>0){ mdr/=mc; mdg/=mc; mdb/=mc; }
    for(int y=qFloor(destImg.y()-R);y<=qCeil(destImg.y()+R);++y){ if(y<0||y>=H)continue;
        for(int x=qFloor(destImg.x()-R);x<=qCeil(destImg.x()+R);++x){ if(x<0||x>=W)continue;
            double dist=std::hypot(x-destImg.x(),y-destImg.y()); if(dist>R) continue;
            double a=falloff(dist/R); if(a<=0.004) continue;                       // drop sub-threshold alpha (no faint halo)
            QColor sc=srcAt(x,y);
            if(m_cloneMode==1){ sc.setRed(qBound(0,int(sc.red()+mdr+0.5),255)); sc.setGreen(qBound(0,int(sc.green()+mdg+0.5),255)); sc.setBlue(qBound(0,int(sc.blue()+mdb+0.5),255)); }
            QColor dc=img.pixelColor(x,y);
            QColor out=QColor::fromRgbF(dc.redF()+(sc.redF()-dc.redF())*a, dc.greenF()+(sc.greenF()-dc.greenF())*a, dc.blueF()+(sc.blueF()-dc.blueF())*a, qMax(dc.alphaF(),a));
            img.setPixelColor(x,y,out);
        } }
}

// Create a white (fully-opaque) luminance mask covering the shape's local bounds, mapped 1:1.
// With t0=1,t1=0 the render uses f=v: white pixels stay opaque, painted-black pixels go transparent.
void Canvas::ensureMask(Shape& s){
    if (s.transp.geom==4 && !s.transp.maskBmp.isNull()) return;
    QRectF lb=s.localBounds(); if(!lb.isValid() || lb.width()<1 || lb.height()<1) return;   // no mask on a degenerate bbox
    int W,H;
    if (s.fill.type==FillType::Bitmap && !s.fill.image.isNull()){ W=s.fill.image.width(); H=s.fill.image.height(); }
    else { double sc=qMin(1.0, 1024.0/qMax(lb.width(),lb.height())); W=qMax(1,int(lb.width()*sc)); H=qMax(1,int(lb.height()*sc)); }
    QImage m(W,H,QImage::Format_ARGB32); m.fill(Qt::white);
    s.transp.maskBmp=m; s.transp.geom=4; if(s.transp.type<=0) s.transp.type=1; s.transp.t0=1.0; s.transp.t1=0.0;
    s.transp.p0=lb.bottomLeft(); s.transp.p1=lb.bottomRight(); s.transp.p2=lb.topLeft();   // img(0,0)->p2(topLeft)
}
bool Canvas::docToMaskPixel(int si, const QPointF& docPt, QPointF& outImg) const {
    if (si<0||si>=m_doc.shapes.size()) return false;
    const Shape& s=m_doc.shapes[si];
    if (s.transp.geom!=4 || s.transp.maskBmp.isNull()) return false;
    const double W=qMax(1,s.transp.maskBmp.width()), H=qMax(1,s.transp.maskBmp.height());
    const QPointF ax=(s.transp.p1-s.transp.p0)/W, ay=(s.transp.p0-s.transp.p2)/H;
    QTransform tex(ax.x(),ax.y(),ay.x(),ay.y(),s.transp.p2.x(),s.transp.p2.y());
    bool ok=false; QTransform inv=tex.inverted(&ok); if(!ok) return false;
    outImg=inv.map(s.transform.inverted().map(docPt));
    return true;
}
void Canvas::maskDab(Shape& s, QPointF destImg, double targetGray){
    QImage& m=s.transp.maskBmp; if(m.isNull()) return;
    if(m.format()!=QImage::Format_ARGB32) m=m.convertToFormat(QImage::Format_ARGB32);
    if(m.isNull()) return;
    const int W=m.width(), H=m.height(); const double R=qMax(1.0,m_maskNib), h=0.5;
    auto falloff=[&](double t)->double{ if(t>=1)return 0; if(t<=h)return 1; double u=(t-h)/qMax(1e-4,1.0-h); return 1.0-(u*u*(3-2*u)); };
    const int tg=qBound(0,int(targetGray+0.5),255);
    for(int y=qFloor(destImg.y()-R);y<=qCeil(destImg.y()+R);++y){ if(y<0||y>=H)continue;
        for(int x=qFloor(destImg.x()-R);x<=qCeil(destImg.x()+R);++x){ if(x<0||x>=W)continue;
            double dist=std::hypot(x-destImg.x(),y-destImg.y()); if(dist>R)continue;
            double a=falloff(dist/R)*0.5; if(a<=0.004) continue;                 // 0.5 = gradual brush flow
            int g0=m.pixelColor(x,y).red(); int g=qBound(0,int(g0+(tg-g0)*a+0.5),255);
            m.setPixelColor(x,y, QColor(g,g,g,255));                             // grayscale luminance mask
        } }
}

// Collect the distinct <text> font families that Qt could NOT resolve to the requested family
// (i.e. it substituted a fallback). Recurses into groups. Used to warn the user that a font the
// document needs isn't available in THIS session — the usual cause is that the font was installed
// after XarQ launched (Qt reads the system font database once, at process start).
static void collectSubstitutedFonts(const QVector<Shape>& v, QSet<QString>& out){
    for(const Shape& s : v){
        if(s.type==ShapeType::Group){ collectSubstitutedFonts(s.children, out); continue; }
        if(s.type!=ShapeType::Text) continue;
        const QString want=s.font.family();
        if(want.isEmpty()) continue;
        if(QFontInfo(s.font).family().compare(want, Qt::CaseInsensitive)!=0) out.insert(want);
    }
}
QStringList Canvas::substitutedFontFamilies(const QVector<Shape>& shapes){
    QSet<QString> missing; collectSubstitutedFonts(shapes, missing);
    QStringList names(missing.begin(), missing.end()); names.sort(Qt::CaseInsensitive);
    return names;
}
void Canvas::warnMissingFonts(const QVector<Shape>& shapes){
    const QStringList names = substitutedFontFamilies(shapes);
    if(names.isEmpty()) return;
    const QString msg = QStringLiteral("Substituted %1 not available in this session: %2 — install %3 and restart XarQ to render exactly.")
        .arg(names.size()==1?"font":"fonts", names.join(", "), names.size()==1?"it":"them");
    // Defer so it lands AFTER the importer's own \"Imported …\" status message (same event-loop turn).
    QTimer::singleShot(0, this, [this,msg]{ emit statusMessage(msg); });
}
bool Canvas::importSvgFile(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    QVector<QRectF> pages;
    QString err; QVector<Shape> shapes=parseSvg(f.readAll(), &err, QFileInfo(path).absolutePath(), &pages);
    if(shapes.isEmpty()) return false;
    warnMissingFonts(shapes);

    // Inkscape 1.2+ multi-page SVG (pages laid out horizontally on the infinite canvas): map each
    // <inkscape:page> onto a real XarQ page (stacked vertically). Only when opening into a fresh document —
    // File ▸ Import into an existing drawing keeps the current flat behaviour. Objects move from their source
    // page's rect into the matching stacked page; the page size is the first page's.
    if(pages.size()>=2 && m_doc.shapes.isEmpty()){
        Document d; d.pageSize=pages[0].size(); d.pageCount=pages.size(); d.pageGap=50;
        const double adv=d.pageSize.height()+d.pageGap;
        for(Shape s : shapes){
            const QPointF ctr=s.docBounds().center();
            int pg=-1; for(int i=0;i<pages.size();++i) if(pages[i].contains(ctr)){ pg=i; break; }   // its home page
            if(pg<0){ double best=1e300; for(int i=0;i<pages.size();++i){ double dd=QLineF(ctr,pages[i].center()).length(); if(dd<best){ best=dd; pg=i; } } }  // off-page → nearest
            if(pg<0) pg=0;
            const QPointF delta=QPointF(0.0, pg*adv) - pages[pg].topLeft();   // source-page → stacked XarQ page
            s.transform = s.transform * QTransform::fromTranslate(delta.x(), delta.y());
            d.addShape(s);
        }
        setDocument(d); refreshAll(); zoomFit();
        return true;
    }

    addShapes(shapes, "Import SVG");   // undo + select
    zoomFit();
    return true;
}

bool Canvas::importVectorFile(const QString& path){
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    QByteArray data=f.readAll();
    // DOS/binary EPS wrapper (also what .ept is): a 30-byte header — magic C5 D0 D3 C6, then u32-LE
    // offset+length for the PostScript section, the WMF preview and the TIFF preview. Unwrap to the
    // PostScript so the EPS parser sees plain text; plain-text EPS files are unaffected.
    QByteArray dosPreview;                       // TIFF/WMF preview carried by a DOS-wrapped EPS
    if(data.size()>=30 && quint8(data[0])==0xC5 && quint8(data[1])==0xD0 && quint8(data[2])==0xD3 && quint8(data[3])==0xC6){
        auto le32=[&](int o)->quint32{ return quint32(quint8(data[o])) | (quint32(quint8(data[o+1]))<<8)
                                            | (quint32(quint8(data[o+2]))<<16) | (quint32(quint8(data[o+3]))<<24); };
        const qint64 off=le32(4), len=le32(8);
        const qint64 tOff=le32(20), tLen=le32(24);      // TIFF preview section
        if(tOff>=30 && tLen>0 && tOff+tLen<=qint64(data.size())) dosPreview=data.mid(int(tOff), int(tLen));
        if(off>=30 && len>0 && off+len<=qint64(data.size())) data=data.mid(int(off), int(len));
    }
    const bool pdfExt = path.endsWith(".pdf",Qt::CaseInsensitive) || path.endsWith(".epdf",Qt::CaseInsensitive) || path.endsWith(".pdfa",Qt::CaseInsensitive);
    // Poppler reads the whole PDF language. A modern .ai is a PDF too. The built-in reader stays as the
    // fallback for a build without it or a file it refuses.
    if((pdfExt || (path.endsWith(".ai",Qt::CaseInsensitive) && data.startsWith("%PDF"))) && xara::pdfVectorAvailable()){
        emit statusMessage(QStringLiteral("Importing PDF… %1").arg(QFileInfo(path).fileName()));
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        struct CursorGuard{ ~CursorGuard(){ QGuiApplication::restoreOverrideCursor(); } } _cg;
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        xara::PdfVectorStats st;
        const QStringList pages = xara::pdfToSvgPages(path, &st);
        if(st.encrypted){ emit statusMessage("This PDF is password protected, so it can't be imported."); return false; }
        if(importVectorSvgPages(pages, QStringLiteral("Import PDF"), QStringLiteral("Imported PDF"), QStringLiteral("poppler"))){
            if(st.totalPages > st.pages) emit statusMessage(QStringLiteral("Imported the first %1 of %2 PDF pages.").arg(st.pages).arg(st.totalPages));
            else if(st.softMasks > 0) emit statusMessage(QStringLiteral("Imported PDF: %1 soft mask(s) were drawn without their mask.").arg(st.softMasks));
            return true;
        }
    }
    Document imported;
    if(pdfExt) imported=importPdf(data);
    else if(path.endsWith(".ai",Qt::CaseInsensitive)) imported=importAi(data);   // modern AI = PDF, classic = EPS
    else if(path.endsWith(".dwg",Qt::CaseInsensitive)) imported=dwgToDocument(path);   // AutoCAD DWG via LibreDWG (reads by path)
    else if(path.endsWith(".dxf",Qt::CaseInsensitive)) imported=importDxf(data);
    else if(path.endsWith(".wmf",Qt::CaseInsensitive)) imported=importWmf(data);
    else if(path.endsWith(".emf",Qt::CaseInsensitive)) imported=importEmf(data);
    else imported=importEps(data);           // .eps
    if(imported.shapes.isEmpty()){
        // No vector content. A raster-only EPS needs a full PostScript interpreter to rasterise, but a
        // DOS-wrapped one usually carries a TIFF preview of exactly that artwork — place it rather than
        // failing outright.
        if(!dosPreview.isEmpty()){
            QImage prev; prev.loadFromData(dosPreview);
            if(!prev.isNull()){
                placeZipComposite(prev, QFileInfo(path).fileName()+QStringLiteral(" (preview)"),
                                  QStringLiteral("Import EPS preview"));
                emit statusMessage("Imported the EPS preview image (no vector content in the PostScript).");
                return true;
            }
        }
        // Raster-only PostScript whose pixels are a plain hex stream (ImageMagick's DisplayImage
        // prologue) can be read without an interpreter.
        { const QImage pras = pdfRasterImage(data);
          if(!pras.isNull()){
              placeZipComposite(pras, QFileInfo(path).fileName(), QStringLiteral("Import PDF image"));
              emit statusMessage("Imported the raster image embedded in the PDF.");
              return true;
          } }
        { const QImage ras = psDisplayImageRaster(data);
          if(!ras.isNull()){
              placeZipComposite(ras, QFileInfo(path).fileName(), QStringLiteral("Import PostScript image"));
              emit statusMessage("Imported the raster image embedded in the PostScript.");
              return true;
          } }
        return false;
    }
    addShapes(imported.shapes, "Import vector");
    zoomFit();
    return true;
}

// The 8 crop handles (doc space) of a rect: 0 TL,1 T,2 TR,3 R,4 BR,5 B,6 BL,7 L.
static QVector<QPointF> cropHandlesDoc(const QRectF& r){
    return { r.topLeft(), {r.center().x(),r.top()}, r.topRight(), {r.right(),r.center().y()},
             r.bottomRight(), {r.center().x(),r.bottom()}, r.bottomLeft(), {r.left(),r.center().y()} };
}
int Canvas::cropHandleAt(const QPointF& viewPt) const {
    if(!m_cropMode) return -1; const auto pts=cropHandlesDoc(m_cropRectDoc);
    for(int i=0;i<pts.size();++i) if((toView(pts[i])-viewPt).manhattanLength()<=8) return i;
    return -1;
}

// Crop only supports an UNROTATED photo whose bitmap maps uprightly onto its rect (image spans the
// rect via p2=TL, p0=BL, p1=BR, transform axis-aligned). This is the imported-photo case; anything
// else (rotated/sheared, a flipped/rotated fill parallelogram, or a non-Rect shape) would corrupt
// under the fractional-rect crop math, so it is rejected — the user can rasterize it first.
static bool isUprightPhoto(const Shape& s){
    if(s.type!=ShapeType::Rect || s.fill.type!=FillType::Bitmap || s.fill.image.isNull()) return false;
    const QTransform& t=s.transform;
    if(qAbs(t.m12())>1e-6 || qAbs(t.m21())>1e-6) return false;        // no rotation/shear
    const QRectF r=s.rect; const double e=0.5;
    return (s.fill.p0-QPointF(r.left(),r.bottom())).manhattanLength()<=e
        && (s.fill.p1-QPointF(r.right(),r.bottom())).manhattanLength()<=e
        && (s.fill.p2-QPointF(r.left(),r.top())).manhattanLength()<=e;
}

// Crop a bitmap object to a fractional sub-rectangle (0..1 of its rect); the cropped content stays
// at the same doc position. Only valid for an upright photo (see isUprightPhoto).
bool Canvas::cropBitmap(int idx, const QRectF& frac){
    if(idx<0||idx>=m_doc.shapes.size()) return false;
    Shape& s=m_doc.shapes[idx];
    if(!isUprightPhoto(s)) return false;
    QRectF f=frac.intersected(QRectF(0,0,1,1)); if(f.width()<=0.002||f.height()<=0.002) return false;
    const QImage& img=s.fill.image;
    QRect pr(int(f.x()*img.width()+0.5), int(f.y()*img.height()+0.5), int(f.width()*img.width()+0.5), int(f.height()*img.height()+0.5));
    pr=pr.intersected(img.rect()); if(pr.width()<1||pr.height()<1) return false;
    pushUndo("Crop photo");
    const QImage cropped=img.copy(pr);
    const QRectF nr(s.rect.x()+f.x()*s.rect.width(), s.rect.y()+f.y()*s.rect.height(), f.width()*s.rect.width(), f.height()*s.rect.height());
    s.rect=nr; s.fill.image=cropped; s.fill.origImage=cropped; s.fill.tiled=false;
    s.fill.p0=QPointF(nr.left(),nr.bottom()); s.fill.p1=QPointF(nr.right(),nr.bottom()); s.fill.p2=QPointF(nr.left(),nr.top());
    syncLiveCopies(); emit documentChanged(); emit selectionChanged(); update();
    return true;
}
bool Canvas::beginCropSelection(){
    int idx=-1; for(int i:m_sel) if(i>=0&&i<m_doc.shapes.size() && isUprightPhoto(m_doc.shapes[i])){ idx=i; break; }
    if(idx<0){ emit statusMessage("On-canvas crop needs an unrotated photo — rasterize a rotated/complex object first."); return false; }
    m_cropMode=true; m_cropShape=idx; m_cropRectDoc=m_doc.shapes[idx].docBounds(); m_cropHandle=-1;
    setCursor(Qt::CrossCursor); emit statusMessage("Drag the handles to crop; Enter to apply, Esc to cancel."); update();
    return true;
}
void Canvas::applyCropMode(){
    if(!m_cropMode) return; const int idx=m_cropShape;
    // Re-validate: the shapes vector may have changed (undo/reorder) since crop began; only crop if
    // the same slot is still an upright photo. Transform is axis-aligned here, so mapRect is exact.
    if(idx>=0 && idx<m_doc.shapes.size() && isUprightPhoto(m_doc.shapes[idx])){
        Shape& s=m_doc.shapes[idx];
        if(s.rect.width()>0 && s.rect.height()>0){
            const QRectF localCrop=s.transform.inverted().mapRect(m_cropRectDoc);   // doc crop → local
            const QRectF frac((localCrop.x()-s.rect.x())/s.rect.width(), (localCrop.y()-s.rect.y())/s.rect.height(),
                              localCrop.width()/s.rect.width(), localCrop.height()/s.rect.height());
            cropBitmap(idx, frac);
        }
    }
    m_cropMode=false; m_cropShape=-1; m_cropHandle=-1; setCursor(Qt::ArrowCursor); update();
}
void Canvas::cancelCropMode(){ m_cropMode=false; m_cropShape=-1; m_cropHandle=-1; setCursor(Qt::ArrowCursor); update(); }

// A "photo" for straighten purposes: a real (non-fractal) bitmap fill. Unlike crop, a rotated/sheared
// frame is fine — straighten resamples the IMAGE CONTENT and leaves the frame untouched.
static bool isPhotoShape(const Shape& s){
    return s.fill.type==FillType::Bitmap && !s.fill.fractal && !s.fill.image.isNull();
}
double Canvas::straightenAngleDeg() const {
    const QPointF d=m_straightenB-m_straightenA;
    if(std::hypot(d.x(),d.y())<1e-6) return 0.0;
    return std::atan2(d.y(), d.x())*180.0/M_PI;
}
bool Canvas::beginStraightenSelection(){
    int idx=-1; for(int i:m_sel) if(i>=0&&i<m_doc.shapes.size() && isPhotoShape(m_doc.shapes[i])){ idx=i; break; }
    if(idx<0){ emit statusMessage("Straighten needs a selected photo."); return false; }
    m_straightenMode=true; m_straightenShape=idx; m_straightenGrab=-1;
    const QRectF b=m_doc.shapes[idx].docBounds();                       // start with a horizontal line across the middle
    m_straightenA=QPointF(b.left()+b.width()*0.18, b.center().y());
    m_straightenB=QPointF(b.right()-b.width()*0.18, b.center().y());
    setCursor(Qt::CrossCursor);
    emit statusMessage("Drag the line onto something that should be level; Enter to apply, Esc to cancel."); update();
    return true;
}
void Canvas::setStraightenLine(const QPointF& aDoc, const QPointF& bDoc){ m_straightenA=aDoc; m_straightenB=bDoc; if(m_straightenMode) update(); }
void Canvas::applyStraightenMode(){
    if(!m_straightenMode){ return; }
    const int idx=m_straightenShape;
    const double deg=straightenAngleDeg();
    if(idx>=0 && idx<m_doc.shapes.size() && isPhotoShape(m_doc.shapes[idx]) && std::fabs(deg)>1e-3){
        Shape& s=m_doc.shapes[idx];
        // Straighten the CURRENTLY-VISIBLE (adjusted) image, not the pristine source, so any brightness/
        // crop/levels edits are preserved (baked in). Fall back to origImage only if image is missing.
        const QImage src = !s.fill.image.isNull() ? s.fill.image : s.fill.origImage;
        if(!src.isNull()){
            const double W=src.width(), H=src.height();
            // The reference angle is measured in DOC space, but we rotate the image in PIXEL space. When the
            // frame's aspect differs from the image's (anisotropic bitmap-fill mapping) angles aren't preserved,
            // so convert the doc-space line direction into image space and level THAT angle instead.
            double rotDeg = deg;
            auto lin=[&](QPointF v){ return s.transform.map(v) - s.transform.map(QPointF(0,0)); };   // linear (no translation) part
            const QPointF ex = lin(s.fill.p1 - s.fill.p0) / qMax(1.0,W);   // doc vector per image x-unit
            const QPointF ey = lin(s.fill.p0 - s.fill.p2) / qMax(1.0,H);   // doc vector per image y-unit
            const double det = ex.x()*ey.y() - ex.y()*ey.x();
            if(std::fabs(det)>1e-9){
                const double dx=std::cos(deg*M_PI/180.0), dy=std::sin(deg*M_PI/180.0);
                const double ix=( ey.y()*dx - ey.x()*dy)/det, iy=(-ex.y()*dx + ex.x()*dy)/det;   // M^-1 · (dx,dy)
                if(std::hypot(ix,iy)>1e-9) rotDeg = std::atan2(iy,ix)*180.0/M_PI;
            }
            // Rotate the content by -rotDeg to level it, scaling about the centre just enough to keep the frame
            // fully covered so no blank corners appear.
            const double th=rotDeg*M_PI/180.0, cc=std::fabs(std::cos(th)), ss=std::fabs(std::sin(th));
            const double k=qMax((W*cc+H*ss)/W, (W*ss+H*cc)/H);
            QImage out(src.size(), QImage::Format_ARGB32_Premultiplied); out.fill(Qt::transparent);
            { QPainter p(&out); p.setRenderHint(QPainter::SmoothPixmapTransform); p.setRenderHint(QPainter::Antialiasing);
              const QPointF c(W/2.0, H/2.0);
              p.translate(c); p.rotate(-rotDeg); p.scale(k,k); p.translate(-c); p.drawImage(0,0,src); }
            pushUndo("Straighten photo");
            s.fill.image=out; s.fill.origImage=out; s.fill.adjust=Fill::Adjust();   // destructive bake; adjustments already folded into `src`
            emit documentChanged(); emit selectionChanged();
        }
    }
    m_straightenMode=false; m_straightenShape=-1; m_straightenGrab=-1; setCursor(Qt::ArrowCursor); update();
}
void Canvas::cancelStraightenMode(){ m_straightenMode=false; m_straightenShape=-1; m_straightenGrab=-1; setCursor(Qt::ArrowCursor); update(); }

void Canvas::dragEnterEvent(QDragEnterEvent* e){ if(e->mimeData()->hasColor()||e->mimeData()->hasImage()||e->mimeData()->hasUrls()) e->acceptProposedAction(); }

bool Canvas::replacePhotoAt(const QPointF& docPt, const QImage& img){
    if(img.isNull()) return false;
    const int i=hitShape(docPt); if(i<0) return false;
    Shape& s=m_doc.shapes[i];
    if(s.fill.type!=FillType::Bitmap || s.fill.fractal || s.fill.image.isNull()) return false;   // only real photos
    pushUndo("Replace photo");
    s.fill.image=img; s.fill.origImage=img; s.fill.adjust=Fill::Adjust();   // swap the image, keep the frame; reset live edits
    m_sel={i}; emit documentChanged(); emit selectionChanged(); update();
    return true;
}
void Canvas::dropEvent(QDropEvent* e){
    const QPointF d = toDoc(e->position());
    if(e->mimeData()->hasColor()){
        const QColor c=qvariant_cast<QColor>(e->mimeData()->colorData());
        const bool line = e->modifiers() & Qt::ShiftModifier;   // Shift-drop targets the line colour
        if(dropColorAt(d, c, line)) e->acceptProposedAction();
        return;
    }
    // Image/file drop: over an existing photo → replace it (keep its frame); else import as a new object.
    QString path;
    if(e->mimeData()->hasUrls()) for(const QUrl& u : e->mimeData()->urls()) if(u.isLocalFile()){ path=u.toLocalFile(); break; }
    QImage img = e->mimeData()->hasImage() ? qvariant_cast<QImage>(e->mimeData()->imageData())
               : (path.isEmpty()? QImage() : QImage(path));
    if(!img.isNull()){
        if(replacePhotoAt(d, img)){ e->acceptProposedAction(); return; }
        if(!path.isEmpty() && importImage(path)){ emit fileImported(path); e->acceptProposedAction(); return; }
        // A raw image with no local file (e.g. dragged from a browser) → drop it as a new photo at the cursor.
        if(placeBitmap(img, d, "Drop image") >= 0){ e->acceptProposedAction(); return; }
    }
    // A non-raster file was dropped (vector / document / e-book): route it by extension to the matching
    // importer instead of silently doing nothing (QImage(path) only handles raster formats).
    if(!path.isEmpty() && dropImportByExtension(path)){ emit fileImported(path); e->acceptProposedAction(); }
}
// Dispatch a dropped file to the importer for its type. Reused by the canvas drop handler.
bool Canvas::dropImportByExtension(const QString& p){
    auto ends=[&](const char* s){ return p.endsWith(QLatin1String(s), Qt::CaseInsensitive); };
    if(ends(".svg"))  return importSvgFile(p);
    if(ends(".pdf")||ends(".epdf")||ends(".pdfa")||ends(".ai")||ends(".eps")||ends(".epsi")||ends(".ept")||ends(".epsf")||ends(".eps2")||ends(".eps3")||ends(".epi")||ends(".ept2")||ends(".ept3")||ends(".ps")||ends(".ps2")||ends(".ps3")||ends(".dxf")||ends(".dwg")) return importVectorFile(p);   // .ps/.epsi = plain PostScript → the EPS path parser
    if(ends(".epub")) return importEpub(p);
    if(ends(".docx")) return importDocx(p);
    if(ends(".fb2")||ends(".fb2.zip")||ends(".fbz")) return importFb2(p);   // FictionBook e-book
    if(ends(".cdr"))  return importCdr(p);
    if(ends(".cmx"))  return importCmx(p);
    if(ends(".rtf"))  return importRtfFile(p);
    if(ends(".cbz"))  return importCbz(p);
    if(ends(".cbr")||ends(".cb7")||ends(".cbt")) return importCbr(p);
    if(ends(".apng")) return importApng(p)||importImage(p);
    if(ends(".odg")||ends(".odp")||ends(".odt")||ends(".ods")||ends(".otg")||ends(".otp")||ends(".ott")||
       ends(".fodg")||ends(".fodp")||ends(".fodt")||ends(".sxd")||ends(".sxi")||ends(".sxw")||ends(".std")||ends(".sti"))
        return importOdg(p);   // all OpenDocument + flat-ODF + OpenOffice.org 1.x → the shared ODF reader
    if(ends(".wpg"))  return importWpg(p);
    if(ends(".vsd")||ends(".vsdx")||ends(".vdx")||ends(".vss")||ends(".vssx")||ends(".vst")||ends(".vstx")||ends(".vsdm")) return importVisio(p);
    if(ends(".fh")||ends(".fh4")||ends(".fh5")||ends(".fh6")||ends(".fh7")||ends(".fh8")||ends(".fh9")||ends(".fh10")||ends(".fh11")||ends(".fhd")||ends(".ft8")||ends(".ft9")||ends(".ft10")||ends(".ft11")) return importFreehand(p);
    if(ends(".pub"))  return importMsPub(p);
    if(ends(".pmd")||ends(".pm6")||ends(".pm5")||ends(".pm4")||ends(".p65")||ends(".t65")) return importPageMaker(p);
    if(ends(".zmf"))  return importZmf(p);
    if(ends(".qxp")||ends(".qxd")||ends(".qxb")||ends(".qxt")) return importQxp(p);
    if(ends(".sda")||ends(".sdd")) return importStarOffice(p);
    if(ends(".key")||ends(".pages")||ends(".numbers")) return importIWork(p);   // Keynote slides / Pages + Numbers text
    if(ends(".cwk")||ends(".drw")) return importMwaw(p);
    if(ends(".dgn")||ends(".tab")||ends(".mif")||ends(".gpkg")||ends(".gml")||ends(".s57")||ends(".000")) return importOgr(p);
    if(ends(".gv")||ends(".dot")) return importDot(p);
    if(ends(".tgs"))  return importTgs(p);
    if(ends(".topojson")) return importTopoJson(p);
    if(ends(".vml"))  return importVml(p);
    if(ends(".xps")||ends(".oxps")) return importXps(p);
    if(ends(".sla")||ends(".scd")) return importScribus(p);
    if(ends(".afdesign")||ends(".afphoto")||ends(".afpub")||ends(".aftemplate")||ends(".afpackage")||ends(".af"))
        return importAffinity(p);   // one container covers every Affinity file type
    if(ends(".ora"))  return importOra(p);
    if(ends(".kra"))  return importKra(p);
    if(ends(".wmf")||ends(".apm")) return importWmfFile(p);   // .apm = Aldus Placeable Metafile (libwmf/native both detect the 0x9AC6CDD7 header)
    if(ends(".geojson")) return importGeoJson(p);
    if(ends(".kml")||ends(".kmz")) return importKml(p);
    if(ends(".plt")||ends(".hpgl")||ends(".hgl")) return importHpgl(p);
    if(ends(".shp"))  return importShapefile(p);
    if(ends(".gpx"))  return importGpx(p);
    if(ends(".fig"))  return importXfig(p);
    if(ends(".cgm"))  return importCgm(p);
    if(ends(".svgz")) return importSvgz(p);
    if(ends(".gbr")||ends(".ger")||ends(".gtl")||ends(".gbl")||ends(".gts")||ends(".gbs")||ends(".gto")||ends(".gbo")||ends(".gko")||ends(".gm1")||ends(".gpt")||ends(".pho")) return importGerber(p);
    if(ends(".drl")||ends(".xnc")||ends(".drd")||ends(".tap")||ends(".exc")||ends(".nc")) return importExcellon(p);
    if(ends(".lottie")) return importLottie(p);
    if(ends(".sketch")) return importSketch(p);
    if(ends(".json")) return importLottie(p)||importGeoJson(p);
    if(ends(".gif"))  return importAnimatedGif(p);
    if(ends(".xar"))  return load(p);          // open a dropped native document
    return importImage(p);                      // last resort: let Qt's image plugins try (covers PSD etc.)
}

// Drag-and-drop a colour onto a shape: dropping inside the filled area sets the flat FILL colour;
// dropping on the outline (or with forceLine) sets the LINE colour — Xara's colour-drop behaviour.
bool Canvas::dropColorAt(const QPointF& docPt, const QColor& c, bool forceLine){
    if(!c.isValid()) return false;
    const int i=hitShape(docPt); if(i<0) return false;
    // With the Shadow tool active, a dropped colour recolours the object's SHADOW (not its fill).
    if(m_tool==Tool::Shadow && m_doc.shapes[i].shadow.on && !forceLine){
        pushUndo("Shadow colour"); m_doc.shapes[i].shadow.color=c; m_sel={i};
        emit documentChanged(); emit selectionChanged(); update(); return true;
    }
    // Likewise the Contour tool: a dropped colour recolours the CONTOUR.
    if(m_tool==Tool::Contour && m_doc.shapes[i].contour.on && !forceLine){
        pushUndo("Contour colour"); m_doc.shapes[i].contour.color=c; m_sel={i};
        emit documentChanged(); emit selectionChanged(); update(); return true;
    }
    // The Bevel tool: a dropped colour sets the bevel LIGHT colour (tints the highlight). Keep the alpha
    // opaque — the highlight strength comes from the lighting model, not the swatch's alpha.
    if(m_tool==Tool::Bevel && m_doc.shapes[i].bevel.on && !forceLine){
        pushUndo("Bevel colour"); m_doc.shapes[i].bevel.lightColor=QColor(c.red(),c.green(),c.blue()); m_sel={i};
        emit documentChanged(); emit selectionChanged(); update(); return true;
    }
    // The Extrude tool: a dropped colour sets the 3-D wall (side/back) colour. Opaque — the shading darkens it.
    if(m_tool==Tool::Extrude && m_doc.shapes[i].extrude.on && !forceLine){
        pushUndo("Extrude colour"); m_doc.shapes[i].extrude.color=QColor(c.red(),c.green(),c.blue()); m_sel={i};
        emit documentChanged(); emit selectionChanged(); update(); return true;
    }
    pushUndo("Drop colour");
    Shape& s=m_doc.shapes[i];
    const QPainterPath dp = s.transform.map(s.localPath());
    const bool interior = s.fill.isVisible() && dp.contains(docPt);
    // A photo/bitmap fill has no meaningful "flat colour" — dropping onto it must NOT replace the
    // image with a solid colour, so route it to the line colour instead.
    const bool photoFill = (s.fill.type==FillType::Bitmap && !s.fill.fractal);
    if(forceLine || !interior || photoFill){ s.stroke.enabled=true; s.stroke.color=c; s.stroke.colorRef=-1; if(s.stroke.width<=0.001) s.stroke.width=1.0; }   // manual recolour unlinks from the palette
    else { s.fill.type=FillType::Solid; s.fill.color=c; s.fill.colorRef=-1; s.fill.fractal=false; }
    m_sel={i};
    syncLiveCopies(); emit documentChanged(); emit selectionChanged(); update();
    return true;
}

// Multi-stage transparency: insert an intermediate stop (position + transparency 0..1) into every
// selected object that already has a graduated transparency. Stops are kept sorted by position.
bool Canvas::addTransparencyStop(double pos, double t){
    Shape* f=firstSelected();
    if(!f || f->transp.type==0 || f->transp.geom==0){ emit statusMessage("Give the object a graduated transparency first (Transparency tool)."); return false; }
    pushUndo("Add transparency stop");
    const TranspStop st{qBound(0.0,pos,1.0), qBound(0.0,t,1.0)};
    for(int i: m_sel){ if(i<0||i>=m_doc.shapes.size()) continue; Shape& sh=m_doc.shapes[i];
        if(sh.transp.type>0 && sh.transp.geom>0){ sh.transp.stops.push_back(st);
            std::sort(sh.transp.stops.begin(), sh.transp.stops.end(), [](const TranspStop&a,const TranspStop&b){ return a.pos<b.pos; }); } }
    syncLiveCopies(); emit documentChanged(); update();
    return true;
}

// Apply a plasma/fractal cloud fill to the selection (Xara's Fractal fill made accessible). The
// generated tile ramps c0→c1 and the render stretches it across each object's bounds.
void Canvas::applyFractalFill(const QColor& c0, const QColor& c1){
    if(m_sel.isEmpty()){ emit statusMessage("Select an object to give it a fractal fill."); return; }
    const QImage img = makeFractalFill(c0, c1);
    applyToSelection([&](Shape& s){
        s.fill.type=FillType::Bitmap; s.fill.fractal=true; s.fill.image=img; s.fill.origImage=img;
        s.fill.color=c0; s.fill.color2=c1; s.fill.tiled=true; s.fill.contone=false;
        // Reset any leftover gradient/mesh state so exporters and non-render code don't mis-read it;
        // the fractal branch stretches the tile across the local bounds via this parallelogram.
        s.fill.stops.clear(); s.fill.meshColors.clear();
        const QRectF lb=s.localBounds();
        s.fill.p0=lb.bottomLeft(); s.fill.p1=lb.bottomRight(); s.fill.p2=lb.topLeft();
    }, "Fractal fill");
}

// Convert a live drop-shadow / contour into editable shapes (manual "Convert to Editable Shapes"):
// the shadow becomes a feathered silhouette Path offset behind, and each contour step becomes a
// stroked ring Path — all placed under the original, whose effect flags are then cleared.
bool Canvas::expandEffectsSelection(){
    if(m_sel.isEmpty()) return false;
    QSet<int> sel(m_sel.begin(), m_sel.end());
    bool any=false;
    QVector<Shape> out; out.reserve(m_doc.shapes.size());
    for(int i=0;i<m_doc.shapes.size();++i){
        Shape s=m_doc.shapes[i];
        const bool hasSh = s.shadow.on;
        const bool hasCo = s.contour.on && s.contour.steps>=1;
        if(sel.contains(i) && (hasSh || hasCo)){
            const QPainterPath dp = s.transform.map(s.localPath());   // doc-space silhouette
            // Shadow first (bottom): a feathered filled silhouette translated by the shadow offset.
            if(hasSh){
                Shape sh; sh.type=ShapeType::Path; sh.name=QStringLiteral("Shadow");
                sh.path=dp.translated(s.shadow.offset); sh.fill.type=FillType::Solid; sh.fill.color=s.shadow.color;
                sh.stroke.enabled=false; sh.feather=s.shadow.blur;
                out.push_back(sh);
            }
            // Contour rings (middle, outer→inner) matching the live render's fade.
            if(hasCo){
                for(int k=s.contour.steps;k>=1;--k){ double w=s.contour.width*k/s.contour.steps;
                    QPainterPathStroker st; st.setWidth(qAbs(w)*2); st.setJoinStyle(Qt::RoundJoin);
                    QPainterPath ring=st.createStroke(dp);
                    QColor col=s.contour.color; col.setAlphaF(col.alphaF()*(1.0-0.4*(k-1.0)/qMax(1,s.contour.steps)));
                    Shape r; r.type=ShapeType::Path; r.name=QStringLiteral("Contour"); r.path=ring;
                    r.fill.type=FillType::Solid; r.fill.color=col; r.stroke.enabled=false;
                    out.push_back(r);
                }
            }
            s.shadow.on=false; s.contour.on=false; any=true;
        }
        out.push_back(s);
    }
    if(!any){ emit statusMessage("Select an object with a shadow or contour to convert."); return false; }
    pushUndo("Convert effects to shapes");
    m_doc.shapes=out; m_sel.clear();
    syncLiveCopies(); emit documentChanged(); emit selectionChanged(); update();
    return true;
}

// Convert to Bitmap: render the selected objects (with their effects) to a raster image at 2×
// and replace them with a single bitmap-fill Rect covering the same doc-space area. Lets photo
// tools apply to vector art, and flattens complex art. Render-safe (user action; XAR path untouched).
bool Canvas::rasterizeSelection(){
    if(m_sel.isEmpty()) return false;
    resetGroupEntry();
    const int srcLayer = (m_sel.first()>=0 && m_sel.first()<m_doc.shapes.size()) ? m_doc.shapes[m_sel.first()].layer : 0;
    QRectF bb; for(int i:m_sel){ if(i<0||i>=m_doc.shapes.size()) continue; const Shape& s=m_doc.shapes[i];
        QRectF db=s.docBounds();
        if(s.shadow.on) db=db.united(db.translated(s.shadow.offset).adjusted(-s.shadow.blur,-s.shadow.blur,s.shadow.blur,s.shadow.blur));
        // expand for effects that render OUTSIDE the geometry, else the raster would clip them
        double m=0;
        if(s.contour.on && s.contour.width>0) m=qMax(m, s.contour.width);
        if(s.feather>0)   m=qMax(m, s.feather);
        if(s.blur>0.4)    m=qMax(m, s.blur*2.0);
        if(s.bevel.on && s.bevel.outer) m=qMax(m, s.bevel.depth);
        if(m>0) db=db.adjusted(-m,-m,m,m);
        if(s.extrude.on) db=db.adjusted(-s.extrude.depth,-s.extrude.depth,s.extrude.depth,s.extrude.depth);
        bb=bb.isNull()?db:bb.united(db); }
    if(bb.isNull() || bb.width()<1 || bb.height()<1) return false;
    const int S=2;
    const int W=qBound(1,int(bb.width()*S),8000), H=qBound(1,int(bb.height()*S),8000);
    QImage img(W,H,QImage::Format_ARGB32_Premultiplied); if(img.isNull()) return false; img.fill(Qt::transparent);
    { QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
      g.scale(double(W)/bb.width(), double(H)/bb.height()); g.translate(-bb.left(),-bb.top());
      Document tmp; tmp.pageSize=m_doc.pageSize; for(int i:m_sel){ if(i>=0&&i<m_doc.shapes.size()) tmp.shapes.push_back(m_doc.shapes[i]); }
      paintDocument(g, tmp); }
    pushUndo("Convert to bitmap");
    Shape s; s.type=ShapeType::Rect; s.name=QStringLiteral("Bitmap"); s.rect=QRectF(0,0,bb.width(),bb.height());
    s.fill.type=FillType::Bitmap; s.fill.image=img; s.fill.origImage=img; s.fill.tiled=false;
    s.fill.p0=QPointF(0,bb.height()); s.fill.p1=QPointF(bb.width(),bb.height()); s.fill.p2=QPointF(0,0);
    s.stroke.enabled=false; s.layer=srcLayer; s.transform=QTransform::fromTranslate(bb.left(),bb.top());
    QVector<int> sel=m_sel; std::sort(sel.begin(),sel.end(),std::greater<int>());
    for(int i:sel) if(i>=0&&i<m_doc.shapes.size()) m_doc.shapes.removeAt(i);
    m_doc.shapes.push_back(s); m_sel={ (int)m_doc.shapes.size()-1 };
    syncLiveCopies(); emit documentChanged(); emit selectionChanged(); update();
    return true;
}

// Create a paintbrush from the selected artwork: rasterise it to a transparent stamp and store it as the
// current stroke's brush ink, so subsequent Freehand/Line strokes paint with that shape (manual "brush from selection").
bool Canvas::createBrushFromSelection(){
    if(m_sel.isEmpty()){ emit statusMessage("Select some artwork to turn into a brush."); return false; }
    QRectF bb; for(int i:m_sel){ if(i<0||i>=m_doc.shapes.size()) continue; QRectF db=m_doc.shapes[i].docBounds(); bb=bb.isNull()?db:bb.united(db); }
    if(bb.isNull()||bb.width()<1||bb.height()<1) return false;
    const double s = qMin(4.0, 512.0/qMax(bb.width(),bb.height()));   // cap the longest edge at ~512 px
    const int W=qBound(1,int(bb.width()*s),1024), H=qBound(1,int(bb.height()*s),1024);
    QImage img(W,H,QImage::Format_ARGB32_Premultiplied); if(img.isNull()) return false; img.fill(Qt::transparent);
    { QPainter g(&img); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
      g.scale(double(W)/bb.width(), double(H)/bb.height()); g.translate(-bb.left(),-bb.top());
      Document tmp; tmp.pageSize=m_doc.pageSize; for(int i:m_sel){ if(i>=0&&i<m_doc.shapes.size()) tmp.shapes.push_back(m_doc.shapes[i]); }
      paintDocument(g, tmp); }
    m_stroke.brushInk=img; m_stroke.brushTint=false; m_stroke.brushScale=0.0; m_stroke.enabled=true;
    emit statusMessage("Brush created from selection — draw with the Freehand or Line tool to paint with it.");
    return true;
}

bool Canvas::traceBitmapSelection(){
    int idx=-1;
    for(int i: m_sel) if(i>=0 && i<m_doc.shapes.size() && m_doc.shapes[i].fill.type==FillType::Bitmap && !m_doc.shapes[i].fill.image.isNull()){ idx=i; break; }
    if(idx<0){ emit statusMessage("Select a photo/bitmap to trace into vector shapes."); return false; }
    const Shape ph=m_doc.shapes[idx];
    QVector<Shape> traced=traceBitmap(ph.fill.image);
    if(traced.isEmpty()){ emit statusMessage("Trace produced no shapes."); return false; }
    const double W = ph.fill.image.width()<1?1:ph.fill.image.width(), H = ph.fill.image.height()<1?1:ph.fill.image.height();
    const Fill& f=ph.fill;   // image-pixel → photo-local (bitmap-fill parallelogram), then → doc via ph.transform
    const QTransform imgToLocal((f.p1.x()-f.p0.x())/W,(f.p1.y()-f.p0.y())/W,(f.p2.x()-f.p0.x())/H,(f.p2.y()-f.p0.y())/H,f.p0.x(),f.p0.y());
    pushUndo("Trace bitmap");
    QVector<int> nsel;
    for(Shape s: traced){ s.transform = s.transform * imgToLocal * ph.transform; s.layer=ph.layer; nsel.push_back(m_doc.addShape(s)); }
    m_sel=nsel;
    emit statusMessage(QString("Traced into %1 vector shapes.").arg(traced.size()));
    emit documentChanged(); emit selectionChanged(); update();
    return true;
}

bool Canvas::stitchSelectedPhotos(){
    QVector<int> idx;
    for(int i: m_sel) if(i>=0 && i<m_doc.shapes.size() && m_doc.shapes[i].fill.type==FillType::Bitmap && !m_doc.shapes[i].fill.image.isNull()) idx.push_back(i);
    if(idx.size()<2){ emit statusMessage("Select two or more overlapping photos to stitch into a panorama."); return false; }
    std::sort(idx.begin(),idx.end(),[&](int a,int b){ return m_doc.shapes[a].docBounds().left() < m_doc.shapes[b].docBounds().left(); });
    QVector<QImage> imgs; for(int i: idx) imgs.push_back(m_doc.shapes[i].fill.image);
    QImage pano=stitchPanorama(imgs);
    if(pano.isNull() || pano.width()<1){ emit statusMessage("Panorama stitch failed."); return false; }
    pushUndo("Panorama stitch");
    const Shape& first=m_doc.shapes[idx.first()]; QRectF db=first.docBounds();
    const double scale = db.width()/qMax(1, first.fill.image.width());      // keep the leftmost photo's doc resolution
    Shape ns; ns.type=ShapeType::Rect; ns.rect=QRectF(0,0,pano.width(),pano.height()); ns.name="Panorama";
    ns.fill.type=FillType::Bitmap; ns.fill.image=pano; ns.fill.origImage=pano;
    ns.fill.p0=QPointF(0,pano.height()); ns.fill.p1=QPointF(pano.width(),pano.height()); ns.fill.p2=QPointF(0,0);
    ns.stroke.enabled=false;
    ns.transform = QTransform::fromScale(scale,scale) * QTransform::fromTranslate(db.left(), db.top());
    std::sort(idx.begin(),idx.end(),std::greater<int>());
    for(int i: idx) m_doc.shapes.removeAt(i);
    m_doc.shapes.push_back(ns); m_sel={int(m_doc.shapes.size())-1};
    emit documentChanged(); emit selectionChanged(); update();
    return true;
}

void Canvas::fitTextToCurve(){
    // Need exactly one Text object and one non-Text "curve" object in the selection.
    int ti=-1, pi=-1; bool ambiguous=false;
    for(int i: m_sel){ if(m_doc.shapes[i].type==ShapeType::Text){ if(ti>=0) ambiguous=true; ti=i; }
                       else { if(pi>=0) ambiguous=true; pi=i; } }
    if(ti<0||pi<0||ambiguous){ emit statusMessage("Select one text object and one curve to fit text to it"); return; }
    pushUndo("Fit text to curve");
    const Shape& ps=m_doc.shapes[pi]; Shape& ts=m_doc.shapes[ti];
    QPainterPath worldPath=ps.transform.map(ps.localPath());   // curve in doc space
    ts.textPath = ts.transform.inverted().map(worldPath);      // store in the text's local space
    m_doc.shapes.removeAt(pi); if(ti>pi) --ti;                 // consume the curve object
    m_sel={ti}; emit documentChanged(); emit selectionChanged(); update();
}
void Canvas::detachTextFromCurve(){
    if(m_sel.size()!=1) return; Shape& s=m_doc.shapes[m_sel.first()];
    if(s.type!=ShapeType::Text || s.textPath.isEmpty()) return;
    pushUndo("Detach text from curve"); s.textPath=QPainterPath(); emit documentChanged(); emit selectionChanged(); update();
}
void Canvas::fitTextInShape(){
    // Flow a Text object's words INSIDE an arbitrary closed shape (manual "text inside a shape").
    int ti=-1, pi=-1; bool ambiguous=false;
    for(int i: m_sel){ if(m_doc.shapes[i].type==ShapeType::Text){ if(ti>=0) ambiguous=true; ti=i; }
                       else { if(pi>=0) ambiguous=true; pi=i; } }
    if(ti<0||pi<0||ambiguous){ emit statusMessage("Select one text object and one closed shape to flow the text inside it"); return; }
    pushUndo("Fit text inside shape");
    const Shape& ps=m_doc.shapes[pi]; Shape& ts=m_doc.shapes[ti];
    const QPainterPath worldPath=ps.transform.map(ps.localPath());  // container in doc space
    ts.textShapePath = ts.transform.inverted().map(worldPath);      // store in the text's local space
    ts.textFrame=true;                                              // enable the wrap layout
    ts.rect=ts.textShapePath.boundingRect();                        // caret/handle geometry follows the region
    m_sel={ti}; emit documentChanged(); emit selectionChanged(); update();   // container stays as a visible backdrop
}
void Canvas::detachTextFromShape(){
    if(m_sel.size()!=1) return; Shape& s=m_doc.shapes[m_sel.first()];
    if(s.type!=ShapeType::Text || s.textShapePath.isEmpty()) return;
    pushUndo("Detach text from shape"); s.textShapePath=QPainterPath(); emit documentChanged(); emit selectionChanged(); update();
}
void Canvas::setTextPathOffset(double d){
    if(m_sel.size()!=1) return; Shape& s=m_doc.shapes[m_sel.first()];
    if(s.type!=ShapeType::Text || s.textPath.isEmpty()) return;
    const double len=s.textPath.length();
    double nd=qBound(0.0, d, qMax(0.0,len-1.0));
    if(qAbs(nd-s.textPathOffset)<1e-6) return;
    pushUndo("Text start offset"); s.textPathOffset=nd; emit documentChanged(); emit selectionChanged(); update();
}
void Canvas::toggleTextPathSide(){
    bool any=false;
    for(int i: m_sel){ if(i<0||i>=m_doc.shapes.size()) continue; Shape& s=m_doc.shapes[i];
        if(s.type!=ShapeType::Text || s.textPath.isEmpty()) continue;
        if(!any){ pushUndo("Flip text side"); any=true; } s.textPathInside=!s.textPathInside; }
    if(any){ emit documentChanged(); emit selectionChanged(); update(); }
}
void Canvas::setDropCapLines(int n){
    if(m_sel.size()!=1) return; Shape& s=m_doc.shapes[m_sel.first()];
    if(s.type!=ShapeType::Text) return;
    int nn=qBound(0, n, 8); if(nn==1) nn=0;              // 1 line isn't a drop cap → off
    if(nn==s.dropCap) return;
    pushUndo("Drop cap"); s.dropCap=nn;
    if(nn>1){ s.textFrame=true; if(s.rect.width()<20) s.rect.setWidth(320); if(s.rect.height()<20) s.rect.setHeight(200); }
    emit documentChanged(); emit selectionChanged(); update();
}
// The drop-cap resize handle sits at the bottom-right corner of the enlarged initial; drag down to add lines.
bool Canvas::dropCapHandle(QPointF& outView) const {
    if(m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel.first()];
    if(s.type!=ShapeType::Text) return false;
    const QRectF cap=s.dropCapRectLocal(); if(cap.isNull()) return false;
    outView=toView(s.transform.map(cap.bottomRight()));
    return true;
}
// The start handle sits ON the baseline at the current offset — drag it along the path to slide the text.
bool Canvas::textPathStartHandle(QPointF& outView) const {
    if(m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel.first()];
    if(s.type!=ShapeType::Text || s.textPath.isEmpty()) return false;
    const double len=s.textPath.length(); if(len<1e-6) return false;
    const double pct=s.textPath.percentAtLength(qBound(0.0,s.textPathOffset,len));
    outView=toView(s.transform.map(s.textPath.pointAtPercent(pct)));
    return true;
}
void Canvas::groupSelection(){ if(m_sel.size()<2)return; pushUndo("Group");
    Shape g; g.type=ShapeType::Group; g.name="Group";
    std::sort(m_sel.begin(),m_sel.end());
    for(int i:m_sel){ Shape c=m_doc.shapes[i]; g.children.push_back(c);}
    std::sort(m_sel.begin(),m_sel.end(),std::greater<int>());
    for(int i:m_sel) m_doc.shapes.removeAt(i);
    m_doc.shapes.push_back(g); m_sel={m_doc.shapes.size()-1};
    emit documentChanged(); emit selectionChanged(); update(); }
// ClipView (Kernel NodeClipViewController): group the selection so the TOPMOST object is the keyhole/window
// whose outline clips every object below it (engine already renders this — see paintShape group branch).
void Canvas::applyClipView(){
    if(m_sel.size()<2) return; pushUndo("ClipView");
    QVector<int> sel=m_sel; std::sort(sel.begin(),sel.end());
    const int topIdx=sel.last();                          // topmost selected = the aperture / keyhole
    Shape g; g.type=ShapeType::Group; g.name="ClipView"; g.isClipView=true;
    g.children.push_back(m_doc.shapes[topIdx]);           // keyhole [0]
    for(int i:sel) if(i!=topIdx) g.children.push_back(m_doc.shapes[i]);   // clipped content [1..], bottom-to-top
    g.clipSplit=1;
    std::sort(sel.begin(),sel.end(),std::greater<int>());
    for(int i:sel) m_doc.shapes.removeAt(i);
    m_doc.shapes.push_back(g); m_sel={(int)m_doc.shapes.size()-1};
    emit documentChanged(); emit selectionChanged(); update();
}
void Canvas::removeClipView(){
    if(m_sel.size()!=1) return; Shape& g=m_doc.shapes[m_sel[0]];
    if(g.type!=ShapeType::Group || !g.isClipView) return;
    pushUndo("Remove ClipView"); g.isClipView=false; g.clipSplit=0;   // becomes a plain group (ungroup to release)
    emit documentChanged(); emit selectionChanged(); update();
}
void Canvas::ungroupSelection(){ if(m_sel.isEmpty())return; resetGroupEntry(); pushUndo("Ungroup"); QVector<int> ns;
    QVector<int> sel=m_sel; std::sort(sel.begin(),sel.end(),std::greater<int>());
    for(int i:sel){ Shape& g=m_doc.shapes[i]; if(g.type!=ShapeType::Group)continue; QTransform gx=g.transform; QVector<Shape> kids=g.children; m_doc.shapes.removeAt(i);
        for(auto k:kids){ k.transform=k.transform*gx; m_doc.shapes.push_back(k); ns.push_back(m_doc.shapes.size()-1);} }
    if(!ns.isEmpty()) m_sel=ns; emit documentChanged(); emit selectionChanged(); update(); }

// Combine selected shapes into one editable Path via boolean ops on their doc-space
// outlines. The result inherits the bottom-most shape's style (Xara convention).
void Canvas::combineSelection(int op){
    if (m_sel.size()<2) return;
    pushUndo("Combine shapes");
    QVector<int> idx=m_sel; std::sort(idx.begin(),idx.end());          // bottom -> top
    auto dpath=[&](int i){ QPainterPath p=Document::docPath(m_doc.shapes[i]); p.setFillRule(Qt::WindingFill); return p; };
    QPainterPath acc=dpath(idx[0]);
    for (int k=1;k<idx.size();++k){ QPainterPath p=dpath(idx[k]);
        switch(op){
            case 0: acc=acc.united(p); break;
            case 1: acc=acc.subtracted(p); break;                       // upper shapes cut the lower
            case 2: acc=acc.intersected(p); break;
            default: acc=acc.united(p).subtracted(acc.intersected(p)); break;  // 3: difference (xor)
        }
    }
    Shape base=m_doc.shapes[idx[0]];
    Shape r; r.type=ShapeType::Path; r.path=acc; r.transform=QTransform();
    r.fill=base.fill; r.stroke=base.stroke; r.opacity=base.opacity; r.blend=base.blend; r.name="Combined";
    r.fill.p0=base.transform.map(base.fill.p0); r.fill.p1=base.transform.map(base.fill.p1);  // gradient -> doc space
    std::sort(idx.begin(),idx.end(),std::greater<int>());
    for (int i:idx) m_doc.shapes.removeAt(i);
    m_doc.shapes.push_back(r); m_sel={m_doc.shapes.size()-1};
    emit documentChanged(); emit selectionChanged(); update();
}

// Bulleted / numbered list: mark selected text frames as a list (each paragraph an item). style 0 off,
// 1 bullet, 2 numbered, 3 lettered; indent = hanging gutter width (0 = default).
void Canvas::setSelectionListStyle(int style, double indent){
    applyToSelection([&](Shape& s){ if(s.type==ShapeType::Text){ s.listStyle=qBound(0,style,3); s.listIndent=qMax(0.0,indent); } }, "List style");
}
// Restart a numbered/lettered list at N (the first item becomes N / the Nth letter); 1 = default.
void Canvas::setSelectionListStart(int start){
    applyToSelection([&](Shape& s){ if(s.type==ShapeType::Text) s.listStart=qMax(1,start); }, "List start");
}
// Apply an arbitrary R×C mesh gradient (grid of colour nodes, row 0 = top) to the selected shapes.
void Canvas::setSelectionMeshGrid(int rows, int cols, const QVector<QColor>& nodes){
    rows=qBound(2,rows,8); cols=qBound(2,cols,8);
    applyToSelection([&](Shape& s){
        if(s.type==ShapeType::Group || s.type==ShapeType::Text) return;
        s.fill.type=FillType::Mesh; s.fill.meshRows=rows; s.fill.meshCols=cols; s.fill.meshColors=nodes; s.fill.colorRef=-1;
        QRectF lb=s.localBounds(); if(lb.isValid() && lb.width()>0 && lb.height()>0){   // span the shape (image(0,0)->p2=topLeft)
            s.fill.p0=lb.bottomLeft(); s.fill.p1=lb.bottomRight(); s.fill.p2=lb.topLeft(); }
    }, "Mesh gradient");
}
// Paragraph spacing: extra vertical space before/after each paragraph on the selected text (single-column).
void Canvas::setSelectionParaSpacing(double before, double after){
    applyToSelection([&](Shape& s){ if(s.type==ShapeType::Text){ s.spaceBefore=qMax(0.0,before); s.spaceAfter=qMax(0.0,after); } }, "Paragraph spacing");
}
// Contour colour graduation: set the outer contour colour (color2) + independent transparency on the
// selected contoured shapes. An invalid outer colour clears the graduation (back to the classic fade).
void Canvas::setSelectionContourGrad(const QColor& outer, double transp){
    applyToSelection([&](Shape& s){ s.contour.on=true; s.contour.color2=outer; s.contour.transp=qBound(0.0,transp,1.0); }, "Contour graduation");
}
// Slice: the UPPER selected shape(s) act as a knife cutting the LOWER (bottom) shape into two pieces —
// the part inside the knife outline and the part outside — both kept with the lower's attributes. The
// knife shapes are removed. (Xara's Combine Shapes ▸ Slice.)
void Canvas::sliceSelection(){
    if(m_sel.size()<2) return;
    pushUndo("Slice");
    QVector<int> idx=m_sel; std::sort(idx.begin(),idx.end());               // bottom → top; idx[0] = the sliced object
    auto dpath=[&](int i){ QPainterPath p=Document::docPath(m_doc.shapes[i]); p.setFillRule(Qt::WindingFill); return p; };
    QPainterPath lower=dpath(idx[0]);
    QPainterPath knife; for(int k=1;k<idx.size();++k) knife=knife.united(dpath(idx[k]));
    const QPainterPath inside=lower.intersected(knife), outside=lower.subtracted(knife);
    const Shape base=m_doc.shapes[idx[0]];
    auto mk=[&](const QPainterPath& pp, const QString& nm)->Shape{ Shape r=base; r.type=ShapeType::Path; r.path=pp; r.transform=QTransform();
        r.fill.p0=base.transform.map(base.fill.p0); r.fill.p1=base.transform.map(base.fill.p1); r.fill.p2=base.transform.map(base.fill.p2);
        r.name=nm; return r; };
    QVector<Shape> pieces;
    if(!outside.isEmpty()) pieces.push_back(mk(outside,"Slice A"));
    if(!inside.isEmpty())  pieces.push_back(mk(inside,"Slice B"));
    if(pieces.isEmpty()) return;
    std::sort(idx.begin(),idx.end(),std::greater<int>());
    for(int i:idx) m_doc.shapes.removeAt(i);
    m_sel.clear(); for(const Shape& p: pieces){ m_doc.shapes.push_back(p); m_sel.push_back(m_doc.shapes.size()-1); }
    emit documentChanged(); emit selectionChanged(); update();
}
// Auto text fields: does any text object use a {page}/{pages}/{date} token?
bool Canvas::hasTextFields() const {
    std::function<bool(const Shape&)> f=[&](const Shape& s)->bool{
        if(s.type==ShapeType::Text && (s.text.contains("{page}")||s.text.contains("{pages}")||s.text.contains("{date}"))) return true;
        for(const Shape& c: s.children) if(f(c)) return true; return false; };
    for(const Shape& s: m_doc.shapes) if(f(s)) return true; return false;
}
// Any top-level text frame that overflows into another (linked/threaded story). Chains are top-level only.
bool Canvas::hasTextChains() const {
    for(const Shape& s: m_doc.shapes) if(s.type==ShapeType::Text && s.nextFrame>=0) return true;
    return false;
}
// Distribute a linked-frame story across its chain, in a doc COPY (d): the head holds the whole story; each
// frame renders only the slice that fits and the remainder flows into shapes[nextFrame], and so on down the chain.
void Canvas::flowTextChains(Document& d) const {
    const int n=d.shapes.size();
    QSet<int> isTarget; for(int i=0;i<n;++i){ int nf=d.shapes[i].nextFrame; if(nf>=0&&nf<n&&d.shapes[i].type==ShapeType::Text) isTarget.insert(nf); }
    for(int i=0;i<n;++i){
        Shape& head=d.shapes[i];
        if(head.type!=ShapeType::Text || head.nextFrame<0 || isTarget.contains(i)) continue;  // only chain heads
        QString remaining=head.text; int idx=i; int guard=0; QSet<int> seen;
        while(idx>=0 && idx<n && guard++<n && !seen.contains(idx)){
            seen.insert(idx);
            Shape& f=d.shapes[idx];
            f.text=remaining;
            f.localPath();                          // populates f.overflowStart for this frame's geometry
            if(f.overflowStart>=0 && f.overflowStart<=remaining.size() && f.nextFrame>=0 && f.nextFrame<n){
                remaining=remaining.mid(f.overflowStart);
                f.text=f.text.left(f.overflowStart);
            } else { remaining.clear(); }           // tail frame (or no overflow): keeps all remaining text
            idx=f.nextFrame;
            // don't break when the story runs out: keep walking so any downstream frames are CLEARED
            // (remaining=="") rather than left showing their own stale text.
        }
    }
}
// A copy of the document with field tokens resolved: {page}=this object's page number, {pages}=page count,
// {date}=today. Used only for rendering/export (the stored tokens are never mutated) → build only when needed.
Document Canvas::resolvedForRender() const {
    Document d=m_doc; const double adv=d.pageSize.height()+d.pageGap;
    const QString today=QDate::currentDate().toString("yyyy-MM-dd");
    std::function<void(Shape&)> f=[&](Shape& s){
        if(s.type==ShapeType::Text && s.text.contains('{')){
            const int pg=qBound(0, adv>0? int(s.docBounds().center().y()/adv):0, qMax(0,d.pageCount-1));
            s.text.replace("{page}",QString::number(pg+1)).replace("{pages}",QString::number(d.pageCount)).replace("{date}",today); }
        for(Shape& c: s.children) f(c); };
    for(Shape& s: d.shapes) f(s);
    flowTextChains(d);                              // linked/threaded text frames: distribute the story across the chain (no-op if none)
    return d;
}
void Canvas::setSelectionFillProfile(double bias, double gain){
    applyToSelection([&](Shape& s){ s.fill.profileBias=qBound(-1.0,bias,1.0); s.fill.profileGain=qBound(-1.0,gain,1.0); }, "Gradient profile");
}
void Canvas::setSelectionTranspProfile(double bias, double gain){
    applyToSelection([&](Shape& s){ s.transp.profileBias=qBound(-1.0,bias,1.0); s.transp.profileGain=qBound(-1.0,gain,1.0); }, "Transparency profile");
}
// Snap the graduated transparency's axis onto the object's FILL gradient axis, so the fade tracks the
// colour graduation (Xara's "match fill" transparency). Adopts a matching geom if the transparency is flat.
void Canvas::matchTranspToFillAxes(){
    applyToSelection([&](Shape& s){
        int wantGeom = s.fill.type==FillType::Radial ? 2 : s.fill.type==FillType::Conical ? 5 :
                       (s.fill.type==FillType::Linear||s.fill.type==FillType::Mesh||s.fill.type==FillType::Square) ? 1 : 0;
        if (wantGeom==0) return;                                  // no gradient fill to match
        if (s.transp.type==0) s.transp.type=1;                    // give a flat/opaque object a (mix) transparency to carry the fade
        if (s.transp.geom==0){ s.transp.geom=wantGeom; s.transp.t0=0.0; s.transp.t1=1.0; }
        s.transp.p0=s.fill.p0; s.transp.p1=s.fill.p1; s.transp.p2=s.fill.p2;
        s.transp.elliptical=(s.fill.type==FillType::Radial);
    }, "Match transparency to fill");
}
// Freehand redraw-over-path (Xara): a stroke that STARTS on the single selected open path reshapes it
// instead of making a new object. If it also ENDS on the path, the spanned section is replaced by the
// stroke; if it starts at an endpoint and ends in space, the path is extended. Returns false (=> commit a
// new Freehand shape) when the gesture doesn't begin on the selected path.
bool Canvas::freehandRedraw(const QVector<QPointF>& strokeDoc){
    if(m_sel.size()!=1 || strokeDoc.size()<2) return false;
    const int idx=m_sel[0]; if(idx<0||idx>=m_doc.shapes.size()) return false;
    Shape& s=m_doc.shapes[idx];
    if(s.type!=ShapeType::Path) return false;
    QList<QPolygonF> polys=Document::docPath(s).toSubpathPolygons();
    if(polys.size()!=1 || polys.first().size()<2) return false;      // only single-subpath paths
    QPolygonF poly=polys.first();
    const bool closed = QLineF(poly.first(),poly.last()).length()<1e-3;
    if(closed && poly.size()>2) poly.removeLast();                    // drop the duplicate closing vertex
    const QPointF a=strokeDoc.first(), b=strokeDoc.last();
    auto nearestV=[&](QPointF p,int& outIdx)->double{ double bd=1e18; int bi=0;
        for(int i=0;i<poly.size();++i){ double dv=QLineF(toView(p),toView(poly[i])).length(); if(dv<bd){bd=dv;bi=i;} }
        outIdx=bi; return bd; };
    int ia,ib; const double da=nearestV(a,ia), db=nearestV(b,ib);
    const double TH=14.0;                                             // screen-px tolerance
    if(da>TH) return false;                                           // stroke must BEGIN on the path
    QVector<QPointF> mid(strokeDoc.begin(), strokeDoc.end());
    QVector<QPointF> out;
    if(db<=TH){                                                       // both ends on the path → replace the span
        const int lo=qMin(ia,ib), hi=qMax(ia,ib);
        if(hi-lo<1) return false;
        QVector<QPointF> head, tail;
        for(int i=0;i<=lo;++i) head.push_back(poly[i]);
        for(int i=hi;i<poly.size();++i) tail.push_back(poly[i]);
        const QPointF hEnd = head.isEmpty()? poly[lo] : head.last();  // orient the stroke to bridge head→tail
        if(!mid.isEmpty() && QLineF(hEnd,mid.first()).length() > QLineF(hEnd,mid.last()).length())
            std::reverse(mid.begin(),mid.end());
        out=head; out+=mid; out+=tail;
    } else {                                                          // ends in space → extend from an endpoint
        if(closed) return false;
        // The stroke must START at a true endpoint (index 0 or the last), not merely near a low/high index —
        // otherwise a 2-vertex line extended from its END node, or a stroke begun mid-path, would misfire.
        if(ia==0){ std::reverse(mid.begin(),mid.end()); out=mid; for(const QPointF& p:poly) out.push_back(p); }        // at START → prepend
        else if(ia==poly.size()-1){ for(const QPointF& p:poly) out.push_back(p); out+=mid; }                          // at END → append
        else return false;                                           // starts mid-path but ends off-path → not a clean gesture
    }
    if(out.size()<2) return false;
    pushUndo("Redraw path");
    QPainterPath np=smoothFreehand(out, m_freeSmooth);
    if(closed) np.closeSubpath();
    s.path=np; s.transform=QTransform();                             // path is now baked in doc space
    emit documentChanged(); emit selectionChanged(); update();
    return true;
}
// Link the selected text frames into one story chain (selection order): the story typed in the first frame
// overflows into the second, then the third, and so on. The last frame ends the chain (nextFrame=-1).
void Canvas::linkTextFrames(){
    QVector<int> frames; for(int i:m_sel) if(i>=0&&i<m_doc.shapes.size()&&m_doc.shapes[i].type==ShapeType::Text) frames.push_back(i);
    if(frames.size()<2) return;
    pushUndo("Link text frames");
    for(int k=0;k<frames.size();++k) m_doc.shapes[frames[k]].nextFrame = (k+1<frames.size())? frames[k+1] : -1;
    update(); emit documentChanged();
}
// Real Table of Contents: collect heading-styled text (textStyleName containing "head"/"title"),
// in page-then-Y order, each with the page it sits on and a dot leader to the page number. If the
// document has no heading styles, fall back to every text shape's first line (legacy behaviour).
QString Canvas::tocText() const {
    struct E{ int page; double y; int level; QString title; };
    QVector<E> entries; const double adv=m_doc.pageSize.height()+m_doc.pageGap;
    QRegularExpression numRe("(\\d+)");
    for(const Shape& s: m_doc.shapes){
        if(s.type!=ShapeType::Text) continue;
        const QString first=s.text.split('\n').first().trimmed();
        if(first.isEmpty() || first.size()>=80) continue;
        const bool isHeading = s.textStyleName.contains("head",Qt::CaseInsensitive) || s.textStyleName.contains("title",Qt::CaseInsensitive);
        int level=1; { auto m=numRe.match(s.textStyleName); if(m.hasMatch()) level=qBound(1,m.captured(1).toInt(),4); }
        const int page=int(s.docBounds().center().y()/(adv>0?adv:1))+1;
        entries.push_back({ page, s.docBounds().top(), isHeading?level:0, first });
    }
    bool anyHeading=false; for(const E& e: entries) if(e.level>0){ anyHeading=true; break; }
    QVector<E> use;
    for(const E& e: entries){ if(anyHeading && e.level==0) continue; use.push_back({e.page,e.y,qMax(1,e.level),e.title}); }
    std::sort(use.begin(),use.end(),[](const E& a,const E& b){ return a.page!=b.page? a.page<b.page : a.y<b.y; });
    QString out="Contents\n"; int num=1;
    for(const E& e: use){ const QString indent(2*(e.level-1),' '); const QString pg=QString::number(e.page);
        QString line=indent + (e.level==1? QString::number(num++)+".  " : QString()) + e.title;
        const int dots=qMax(3, 52 - line.size() - pg.size());
        out += line + " " + QString(dots,'.') + " " + pg + "\n"; }
    return out;
}
// Add an explicit tab stop to the selected text (kept sorted by position). Non-empty tabStops make \t
// use per-stop alignment + leaders instead of the regular tabWidth interval.
void Canvas::addTabStopToSelection(double pos, int align, QChar leader){
    applyToSelection([&](Shape& s){ if(s.type!=ShapeType::Text) return;
        TabStop t; t.pos=pos; t.align=qBound(0,align,3); t.leader=leader; s.tabStops.push_back(t);
        std::sort(s.tabStops.begin(),s.tabStops.end(),[](const TabStop& a,const TabStop& b){ return a.pos<b.pos; }); }, "Add tab stop");
}
void Canvas::clearTabStopsOnSelection(){
    applyToSelection([&](Shape& s){ if(s.type==ShapeType::Text) s.tabStops.clear(); }, "Clear tab stops");
}
// Full-width background band: toggle webBand on the selection. If any selected shape isn't a band yet,
// turn them all on; otherwise turn them all off. On Publish, a band shape becomes a 100vw stripe.
void Canvas::toggleWebBand(){
    if(m_sel.isEmpty()) return;
    bool anyOff=false; for(int i:m_sel) if(i>=0&&i<m_doc.shapes.size() && !m_doc.shapes[i].webBand) anyOff=true;
    applyToSelection([anyOff](Shape& s){ s.webBand=anyOff; }, "Full-width band");
}
// Break the chain: clear nextFrame on every selected text frame (each becomes an independent frame again).
void Canvas::unlinkTextFrames(){
    bool any=false; for(int i:m_sel) if(i>=0&&i<m_doc.shapes.size()&&m_doc.shapes[i].type==ShapeType::Text&&m_doc.shapes[i].nextFrame>=0) any=true;
    if(!any) return;
    pushUndo("Unlink text frames");
    for(int i:m_sel) if(i>=0&&i<m_doc.shapes.size()&&m_doc.shapes[i].type==ShapeType::Text) m_doc.shapes[i].nextFrame=-1;
    update(); emit documentChanged();
}
// Split a QPainterPath into its subpaths, PRESERVING bezier curves (unlike toSubpathPolygons, which flattens).
static QVector<QPainterPath> splitSubpaths(const QPainterPath& p){
    QVector<QPainterPath> out; QPainterPath cur;
    for(int i=0;i<p.elementCount();++i){ const QPainterPath::Element e=p.elementAt(i);
        if(e.type==QPainterPath::MoveToElement){ if(cur.elementCount()>0) out.push_back(cur); cur=QPainterPath(); cur.moveTo(e.x,e.y); }
        else if(e.type==QPainterPath::LineToElement) cur.lineTo(e.x,e.y);
        else if(e.type==QPainterPath::CurveToElement && i+2<p.elementCount()){
            const QPainterPath::Element c2=p.elementAt(i+1), en=p.elementAt(i+2);
            cur.cubicTo(e.x,e.y, c2.x,c2.y, en.x,en.y); i+=2; } }
    if(cur.elementCount()>0) out.push_back(cur);
    return out;
}
// Break Apart: split each selected shape whose outline has ≥2 disjoint subpaths into separate Shape objects
// (each subpath its own shape, inheriting all attributes). The inverse of Join Shapes.
void Canvas::breakApartSelection(){
    if(m_sel.isEmpty()) return;
    QVector<Shape> pieces; QVector<int> toRemove;
    for(int i:m_sel){ if(i<0||i>=m_doc.shapes.size()) continue; const Shape& s=m_doc.shapes[i];
        if(s.type==ShapeType::Group) continue;
        const QPainterPath lp = (s.type==ShapeType::Path)? s.path : s.localPath();
        const QVector<QPainterPath> subs=splitSubpaths(lp);
        if(subs.size()<2) continue;
        toRemove.push_back(i);
        for(const QPainterPath& sp:subs){ Shape ns=s; ns.type=ShapeType::Path; ns.path=sp; ns.name=s.name+" piece"; pieces.push_back(ns); } }
    if(toRemove.isEmpty()){ emit statusMessage("Select a shape made of separate sub-paths to break apart."); return; }
    pushUndo("Break apart");
    std::sort(toRemove.begin(),toRemove.end(),std::greater<int>());
    for(int i:toRemove) m_doc.shapes.removeAt(i);
    QVector<int> nsel; for(const Shape& p:pieces){ m_doc.shapes.push_back(p); nsel.push_back(m_doc.shapes.size()-1); }
    m_sel=nsel; emit documentChanged(); emit selectionChanged(); update();
    emit statusMessage(QString("Broke apart into %1 shapes.").arg(pieces.size()));
}
// Join Shapes: merge the selected shapes' outlines into ONE compound Path (subpaths concatenated, even-odd
// so overlaps punch holes) — distinct from Combine▸Add (a boolean union). Keeps the bottom shape's attributes.
void Canvas::joinShapesSelection(){
    if(m_sel.size()<2) return;
    pushUndo("Join shapes");
    QVector<int> idx=m_sel; std::sort(idx.begin(),idx.end());          // bottom -> top
    QPainterPath acc; acc.setFillRule(Qt::OddEvenFill);
    for(int i:idx) acc.addPath(Document::docPath(m_doc.shapes[i]));
    Shape base=m_doc.shapes[idx[0]];
    Shape r; r.type=ShapeType::Path; r.path=acc; r.transform=QTransform();
    r.fill=base.fill; r.stroke=base.stroke; r.opacity=base.opacity; r.blend=base.blend; r.name="Joined";
    r.fill.p0=base.transform.map(base.fill.p0); r.fill.p1=base.transform.map(base.fill.p1); r.fill.p2=base.transform.map(base.fill.p2);
    std::sort(idx.begin(),idx.end(),std::greater<int>());
    for(int i:idx) m_doc.shapes.removeAt(i);
    m_doc.shapes.push_back(r); m_sel={m_doc.shapes.size()-1};
    emit documentChanged(); emit selectionChanged(); update();
}

// Bake selected primitives (rect/ellipse/polygon/text) into editable Paths in place.
void Canvas::convertToPath(){
    if (m_sel.isEmpty()) return; pushUndo("Convert to path");
    for (int i:m_sel){ Shape& s=m_doc.shapes[i];
        if (s.type==ShapeType::Path || s.type==ShapeType::Group) continue;
        s.path=s.localPath(); s.type=ShapeType::Path; }
    emit documentChanged(); emit selectionChanged(); update();
}

// ---- cascading named text styles ----
static int findTextStyle(const Document& d, const QString& name){ for(int i=0;i<d.textStyles.size();++i) if(d.textStyles[i].name==name) return i; return -1; }
static void setTextAttrs(Shape& s, const TextStyle& st){ s.font=st.font; s.fill.type=FillType::Solid; s.fill.color=st.color; s.textAlign=st.align; s.textStyleName=st.name; }
static void applyStyleToShape(Shape& s, const TextStyle& st){ if(s.type==ShapeType::Text) setTextAttrs(s,st); for(Shape& c:s.children) applyStyleToShape(c,st); }

void Canvas::createTextStyle(const QString& name){
    if(name.isEmpty()) return;
    Shape* s=firstSelected(); if(!s || s->type!=ShapeType::Text){ emit statusMessage("Select a text object to define a style from."); return; }
    pushUndo("Create text style");
    TextStyle st; st.name=name; st.font=s->font; st.color=(s->fill.type==FillType::Solid)?s->fill.color:QColor(Qt::black); st.align=s->textAlign;
    const int idx=findTextStyle(m_doc,name); if(idx>=0) m_doc.textStyles[idx]=st; else m_doc.textStyles.push_back(st);
    for(int i:m_sel) if(i>=0&&i<m_doc.shapes.size()) applyStyleToShape(m_doc.shapes[i], st);
    emit documentChanged(); emit selectionChanged(); update();
}
void Canvas::applyTextStyle(const QString& name){
    const int idx=findTextStyle(m_doc,name); if(idx<0||m_sel.isEmpty()) return;
    pushUndo("Apply text style"); const TextStyle st=m_doc.textStyles[idx];
    for(int i:m_sel) if(i>=0&&i<m_doc.shapes.size()) applyStyleToShape(m_doc.shapes[i], st);
    emit documentChanged(); emit selectionChanged(); update();
}
void Canvas::updateTextStyle(const QString& name, const QFont& f, const QColor& c, int align){
    const int idx=findTextStyle(m_doc,name); if(idx<0) return;
    pushUndo("Edit text style");
    TextStyle& stref=m_doc.textStyles[idx]; stref.font=f; stref.color=c; stref.align=align;
    const TextStyle st=stref;   // re-sync EVERY text shape (recursing groups) whose textStyleName == name (cascade)
    std::function<void(Shape&)> sync=[&](Shape& sh){ if(sh.type==ShapeType::Text && sh.textStyleName==name) setTextAttrs(sh, st); for(Shape& ch:sh.children) sync(ch); };
    for(Shape& sh:m_doc.shapes) sync(sh);
    emit documentChanged(); emit selectionChanged(); update();
}

// Attach raw web-embed HTML (iframe/video/map/form) to the selection, or drop a placeholder
// rectangle carrying it if nothing is selected. The object renders as a plain box on the canvas
// and becomes the live embed on web publish.
void Canvas::setSelectionEmbed(const QString& html){
    if(html.isEmpty()) return;
    if(m_sel.isEmpty()){
        pushUndo("Web embed");
        Shape s; s.type=ShapeType::Rect; s.name=QStringLiteral("Embed"); s.rect=QRectF(0,0,320,200);
        s.fill.type=FillType::Solid; s.fill.color=QColor(235,238,245);
        s.stroke.enabled=true; s.stroke.color=QColor(150,160,180); s.stroke.width=1.0; s.webEmbed=html;
        s.transform=QTransform::fromTranslate(m_doc.pageSize.width()*0.2, m_doc.pageSize.height()*0.2);
        m_doc.shapes.push_back(s); m_sel={ (int)m_doc.shapes.size()-1 };
        emit documentChanged(); emit selectionChanged(); update();
    } else applyToSelection([&](Shape& s){ s.webEmbed=html; }, "Web embed");
}
// Mouse-over pop-up: the first selected object becomes a trigger, the last a pop-up panel (hidden until
// the trigger is hovered), paired by a fresh id. On Publish Website a small JS shows/hides the panel.
void Canvas::linkPopup(){
    if(m_sel.size()<2) return;
    int id=1; for(const Shape& s: m_doc.shapes) id=qMax(id, qMax(s.webPopupId,s.webPopupTrigger)+1);
    pushUndo("Mouse-over pop-up");
    if(m_sel.first()>=0 && m_sel.first()<m_doc.shapes.size()) m_doc.shapes[m_sel.first()].webPopupTrigger=id;
    if(m_sel.last()>=0  && m_sel.last()<m_doc.shapes.size())  m_doc.shapes[m_sel.last()].webPopupId=id;
    update(); emit documentChanged();
}
// Contact-form widget: attach a form spec ("Label|type" lines) + optional action URL to the selection,
// or drop a new form placeholder when nothing is selected.
void Canvas::setSelectionForm(const QString& spec, const QString& action){
    if(spec.isEmpty()) return;
    if(m_sel.isEmpty()){
        pushUndo("Web form");
        Shape s; s.type=ShapeType::Rect; s.name=QStringLiteral("Form"); s.rect=QRectF(0,0,340,260);
        s.fill.type=FillType::None; s.stroke.enabled=false; s.webForm=spec; s.url=action;
        s.transform=QTransform::fromTranslate(m_doc.pageSize.width()*0.2, m_doc.pageSize.height()*0.2);
        m_doc.shapes.push_back(s); m_sel={ (int)m_doc.shapes.size()-1 };
        emit documentChanged(); emit selectionChanged(); update();
    } else applyToSelection([&](Shape& s){ s.webForm=spec; if(!action.isEmpty()) s.url=action; }, "Web form");
}

// Pop-up photo (lightbox): mark the selected object(s) so their published image opens full-screen when clicked.
void Canvas::setSelectionLightbox(bool on){
    if(m_sel.isEmpty()) return;
    applyToSelection([&](Shape& s){ s.webLightbox=on; }, on?"Pop-up photo on":"Pop-up photo off");
}
bool Canvas::selectionLightbox() const {
    for(int i: m_sel) if(i>=0 && i<m_doc.shapes.size() && m_doc.shapes[i].webLightbox) return true;
    return false;
}

// Web reveal animation: attach a scroll-into-view animation (kind 0..7) to the selected object(s).
void Canvas::setSelectionWebAnim(int kind, double delay){
    if(m_sel.isEmpty()) return;
    applyToSelection([&](Shape& s){ s.webAnim=kind; s.webAnimDelay=qMax(0.0,delay); }, "Web animation");
}
int Canvas::selectionWebAnim() const {
    for(int i: m_sel) if(i>=0 && i<m_doc.shapes.size() && m_doc.shapes[i].webAnim) return m_doc.shapes[i].webAnim;
    return 0;
}

// Image slider/carousel: group the selected objects into one cross-fading carousel on web publish.
void Canvas::makeSliderFromSelection(){
    if(m_sel.size()<2) return;                                  // a slider needs at least two slides
    int id=0; for(const Shape& s: m_doc.shapes) id=qMax(id,s.webSlider);
    ++id;
    applyToSelection([&](Shape& s){ s.webSlider=id; }, "Make slider");
}
void Canvas::clearSelectionSlider(){
    if(m_sel.isEmpty()) return;
    applyToSelection([&](Shape& s){ s.webSlider=0; }, "Ungroup slider");
}
int Canvas::selectionSlider() const {
    for(int i: m_sel) if(i>=0 && i<m_doc.shapes.size() && m_doc.shapes[i].webSlider) return m_doc.shapes[i].webSlider;
    return 0;
}

// FlexGroup: group the selected objects into a FLUID flex-wrap row that reflows on narrow screens
// (published as a section below the fixed stage — the only place real reflow is possible).
void Canvas::makeFlexFromSelection(){
    if(m_sel.size()<2) return;
    int id=0; for(const Shape& s: m_doc.shapes) id=qMax(id,s.webFlex);
    ++id;
    applyToSelection([&](Shape& s){ s.webFlex=id; }, "Make FlexGroup");
}
void Canvas::clearSelectionFlex(){
    if(m_sel.isEmpty()) return;
    applyToSelection([&](Shape& s){ s.webFlex=0; }, "Ungroup FlexGroup");
}
int Canvas::selectionFlex() const {
    for(int i: m_sel) if(i>=0 && i<m_doc.shapes.size() && m_doc.shapes[i].webFlex) return m_doc.shapes[i].webFlex;
    return 0;
}

// Embedded media: attach video/audio/GIF bytes to the selection (or drop a new placeholder). On web
// publish the placeholder becomes a native <video>/<audio> player (or animated <img> for a GIF).
void Canvas::setSelectionMedia(const QByteArray& data, const QString& mime){
    if(data.isEmpty()||mime.isEmpty()) return;
    if(m_sel.isEmpty()){
        pushUndo("Insert media");
        const bool audio = mime.startsWith("audio");
        Shape s; s.type=ShapeType::Rect; s.name = audio? QStringLiteral("Audio") : QStringLiteral("Media");
        s.rect = audio? QRectF(0,0,320,54) : QRectF(0,0,480,270);
        s.fill.type=FillType::Solid; s.fill.color=QColor(28,30,36);
        s.stroke.enabled=true; s.stroke.color=QColor(90,100,120); s.stroke.width=1.0;
        s.webMediaData=data; s.webMediaMime=mime;
        s.transform=QTransform::fromTranslate(m_doc.pageSize.width()*0.2, m_doc.pageSize.height()*0.2);
        m_doc.shapes.push_back(s); m_sel={ (int)m_doc.shapes.size()-1 };
        emit documentChanged(); emit selectionChanged(); update();
    } else applyToSelection([&](Shape& s){ s.webMediaData=data; s.webMediaMime=mime; }, "Attach media");
}

// Navigation bar: mark the selection as a nav bar (or drop a new full-width bar at the top of the page).
// On Publish Website it becomes a styled <nav> linking every page of the site.
void Canvas::addNavBar(){
    pushUndo("Navigation bar");
    Shape s; s.type=ShapeType::Rect; s.name=QStringLiteral("Navigation Bar");
    s.rect=QRectF(0,0, qMax(200.0,m_doc.pageSize.width()), 48);
    s.fill.type=FillType::Solid; s.fill.color=QColor(34,38,48);
    s.stroke.enabled=false; s.webNavBar=true; s.webStretch=1;    // stretch full width on publish
    s.transform=QTransform::fromTranslate(0,0);
    m_doc.shapes.push_back(s); m_sel={ (int)m_doc.shapes.size()-1 };
    emit documentChanged(); emit selectionChanged(); update();
}

// Set the line-spacing (leading, a multiple of the font height) and letter-spacing (tracking, px)
// of every selected Text object.
void Canvas::setTextSpacing(double lineSpacing, double tracking){
    applyToSelection([&](Shape& s){ if(s.type==ShapeType::Text){ s.lineSpacing=qBound(0.3,lineSpacing,4.0); s.tracking=qBound(-20.0,tracking,50.0); } }, "Text spacing");
}

// Paragraph indents (doc px): first-line offset, left margin (all lines), right margin (narrows the width).
void Canvas::setTextIndents(double firstLine, double left, double right){
    applyToSelection([&](Shape& s){ if(s.type==ShapeType::Text){
        s.firstLineIndent=qBound(-2000.0,firstLine,2000.0); s.leftIndent=qBound(0.0,left,2000.0); s.rightIndent=qBound(0.0,right,2000.0); } }, "Text indents");
}
// Tab-stop interval (doc px); 0 = off. \t in the text advances to the next stop.
void Canvas::setTextTabWidth(double w){
    applyToSelection([&](Shape& s){ if(s.type==ShapeType::Text) s.tabWidth=qBound(0.0,w,4000.0); }, "Tab stops");
}
// Responsive: hide the selected element(s) below a viewport width (px); 0 = always show.
void Canvas::setSelectionHideBelow(int px){
    applyToSelection([&](Shape& s){ s.webHideBelow=qMax(0,px); }, "Responsive visibility");
}

// Website Variant: give the selection an alternate position/size applied below `belowPx` viewport width.
// belowPx<=0 clears the variant. x/y are absolute doc coords (top-left), w is width (px).
void Canvas::setSelectionVariant(int belowPx, double x, double y, double w){
    applyToSelection([&](Shape& s){ s.webVarBelow=qMax(0,belowPx); s.webVarX=x; s.webVarY=y; s.webVarW=w; }, "Website variant");
}
bool Canvas::selectionVariant(int& belowPx, double& x, double& y, double& w) const {
    for(int i: m_sel) if(i>=0 && i<m_doc.shapes.size()){ const Shape& s=m_doc.shapes[i];
        belowPx=s.webVarBelow; x=s.webVarX; y=s.webVarY; w=s.webVarW; return true; }
    return false;
}

// Buttons & Menus: attach an authored dropdown menu tree to the selection (which becomes a nav bar on publish).
void Canvas::setSelectionMenuTree(const QString& tree){
    applyToSelection([&](Shape& s){ s.webMenuTree=tree; if(!tree.trimmed().isEmpty()) s.webNavBar=true; }, "Menu bar");
}
QString Canvas::selectionMenuTree() const {
    for(int i: m_sel) if(i>=0 && i<m_doc.shapes.size()) return m_doc.shapes[i].webMenuTree;
    return QString();
}

// Format a character range of the selected Text object: resolve to per-character attrs, apply
// the overrides, then re-coalesce into contiguous runs. Robust to text edits (ranges clamped).
// Rebuild s.textRuns from per-character attribute arrays, coalescing equal-attribute neighbours into runs.
static void coalesceTextRuns(Shape& s, const QVector<QFont>& fonts, const QVector<QColor>& colors,
                             const QVector<QString>& urls, const QVector<double>& bl){
    const int n=s.text.size(); QVector<TextRun> runs; int i=0;
    while(i<n){ int j=i+1; while(j<n && fonts[j]==fonts[i] && colors[j]==colors[i] && urls[j]==urls[i] && bl[j]==bl[i]) ++j;
        TextRun r; r.start=i; r.len=j-i; r.font=fonts[i]; r.color=colors[i]; r.url=urls[i]; r.baseline=bl[i];
        if(fonts[i].letterSpacingType()==QFont::AbsoluteSpacing) r.tracking=fonts[i].letterSpacing(); runs.push_back(r); i=j; }
    s.textRuns=runs;
}

void Canvas::setTextRunAttr(int start, int len, const QFont* font, const QColor* color, const QString* url){
    Shape* s=firstSelected(); if(!s || s->type!=ShapeType::Text) { emit statusMessage("Select a text object first."); return; }
    const int n=s->text.size(); if(n==0) return;
    start=qBound(0,start,n); int end=qBound(start,start+len,n); if(end<=start) return;
    QVector<QFont> fonts; QVector<QColor> colors; QVector<QString> urls;
    s->resolveChars(fonts,colors,urls);
    const QVector<double> bl=s->resolveBaselines();   // preserve any baseline shift across a font/colour edit
    for(int i=start;i<end;++i){ if(font) fonts[i]=*font; if(color) colors[i]=*color; if(url) urls[i]=*url; }
    pushUndo("Format text");
    coalesceTextRuns(*s,fonts,colors,urls,bl);
    syncLiveCopies(); emit documentChanged(); emit selectionChanged(); update();
}

// Inkscape-style per-selection formatting: while editing text on-canvas with a selection, apply `mut` to each
// selected character's font/colour and re-coalesce runs. No selection (or not editing) → returns false so the
// caller falls back to formatting the whole object.
bool Canvas::formatEditSelection(const std::function<void(QFont&,QColor&)>& mut, const QString& label){
    if(!(editingText() && textHasSelection())) return false;
    Shape& s=m_doc.shapes[m_textEdit]; const int n=s.text.size(); if(n==0) return false;
    int a=qBound(0,qMin(m_caret,m_caretAnchor),n), b=qBound(0,qMax(m_caret,m_caretAnchor),n); if(b<=a) return false;
    QVector<QFont> fonts; QVector<QColor> colors; QVector<QString> urls; s.resolveChars(fonts,colors,urls);
    const QVector<double> bl=s.resolveBaselines();
    for(int i=a;i<b;++i) mut(fonts[i],colors[i]);
    pushUndo(label);
    coalesceTextRuns(s,fonts,colors,urls,bl);
    syncLiveCopies(); emit documentChanged(); emit selectionChanged(); update();
    return true;
}

// Resolved font of the first selected character (for the InfoBar to reflect / toggle the selection's state).
QFont Canvas::editSelectionFont() const {
    if(editingText()){ const Shape& s=m_doc.shapes[m_textEdit];
        int a=qMax(0,qMin(m_caret, m_caretAnchor>=0?m_caretAnchor:m_caret));
        QVector<QFont> fonts; QVector<QColor> colors; QVector<QString> urls; s.resolveChars(fonts,colors,urls);
        if(a<fonts.size()) return fonts[a];
        if(!fonts.isEmpty()) return fonts.back();
        return s.font; }
    return QFont();
}

// Set per-run typography (baseline shift + letter-spacing) over a character range. baseline>0 raises
// (superscript), <0 lowers (subscript); tracking is extra letter-spacing in doc px. Coalesces to runs.
void Canvas::setTextRunTypography(int start, int len, double baseline, double tracking){
    Shape* s=firstSelected(); if(!s || s->type!=ShapeType::Text) { emit statusMessage("Select a text object first."); return; }
    const int n=s->text.size(); if(n==0) return;
    start=qBound(0,start,n); int end=qBound(start,start+len,n); if(end<=start) return;
    QVector<QFont> fonts; QVector<QColor> colors; QVector<QString> urls;
    s->resolveChars(fonts,colors,urls);                 // fonts carry any existing per-run tracking
    QVector<double> bl=s->resolveBaselines();
    for(int i=start;i<end;++i){ bl[i]=baseline; if(tracking!=0.0) fonts[i].setLetterSpacing(QFont::AbsoluteSpacing,tracking);
        else fonts[i].setLetterSpacing(QFont::PercentageSpacing,100.0); }
    pushUndo("Text typography");
    QVector<TextRun> runs; int i=0;
    while(i<n){ int j=i+1; while(j<n && fonts[j]==fonts[i] && colors[j]==colors[i] && urls[j]==urls[i] && bl[j]==bl[i]) ++j;
        TextRun r; r.start=i; r.len=j-i; r.font=fonts[i]; r.color=colors[i]; r.url=urls[i]; r.baseline=bl[i];
        if(fonts[i].letterSpacingType()==QFont::AbsoluteSpacing) r.tracking=fonts[i].letterSpacing(); runs.push_back(r); i=j; }
    s->textRuns=runs;
    syncLiveCopies(); emit documentChanged(); emit selectionChanged(); update();
}

// Coalesce a fully-resolved per-character run list into contiguous runs; if every
// character shares one font+colour and has no link, return empty (a clean single-style
// object uses the shape's base font/fill rather than a redundant single run).
static QVector<TextRun> coalesceRuns(const QVector<QFont>& fonts, const QVector<QColor>& colors, const QVector<QString>& urls){
    QVector<TextRun> runs; const int n=fonts.size(); int i=0;
    while(i<n){ int j=i+1; while(j<n && fonts[j]==fonts[i] && colors[j]==colors[i] && urls[j]==urls[i]) ++j;
        TextRun r; r.start=i; r.len=j-i; r.font=fonts[i]; r.color=colors[i]; r.url=urls[i]; runs.push_back(r); i=j; }
    if(runs.size()==1 && runs[0].url.isEmpty()) return {};
    return runs;
}

// Build a Text shape (with per-character runs) from an HTML fragment via QTextDocument,
// which handles the messy parsing (nested tags, entities, inline styles). Each text
// fragment contributes a run carrying its font, foreground colour and (if a link) href.
Shape Canvas::shapeFromHtml(const QString& html){
    Shape s; s.type=ShapeType::Text; s.name="Text";
    QTextDocument doc; doc.setHtml(html);
    QString plain; QVector<QFont> fonts; QVector<QColor> colors; QVector<QString> urls;
    bool firstBlock=true;
    for(QTextBlock b=doc.begin(); b.isValid(); b=b.next()){
        if(!firstBlock){ plain+='\n'; fonts.push_back(QFont()); colors.push_back(QColor(Qt::black)); urls.push_back(QString()); }
        firstBlock=false;
        for(QTextBlock::iterator it=b.begin(); !it.atEnd(); ++it){
            QTextFragment frag=it.fragment(); if(!frag.isValid()) continue;
            const QString ft=frag.text(); if(ft.isEmpty()) continue;
            QTextCharFormat fmt=frag.charFormat();
            QFont f=fmt.font();
            QColor col=(fmt.foreground().style()!=Qt::NoBrush)?fmt.foreground().color():QColor(Qt::black);
            QString href=fmt.isAnchor()?fmt.anchorHref():QString();
            for(const QChar ch: ft){ plain+=ch; fonts.push_back(f); colors.push_back(col); urls.push_back(href); }
        }
    }
    if(plain.isEmpty()) return s;   // nothing usable
    s.text=plain;
    s.font=fonts.isEmpty()?s.font:fonts.first();
    s.fill.type=FillType::Solid; s.fill.color=colors.isEmpty()?QColor(Qt::black):colors.first();
    s.textRuns=coalesceRuns(fonts,colors,urls);   // empty when the whole fragment is one uniform style
    return s;
}

// Minimal RTF → Text shape. Handles the common word-processor output: \b \i toggles,
// \fs (half-point size), \cf (index into \colortbl), \par newlines, \'hh byte escapes,
// and skips the {\fonttbl…}/{\colortbl…}/{\*…} header groups. Not a full RTF reader.
Shape Canvas::shapeFromRtf(const QByteArray& rtf){
    Shape s; s.type=ShapeType::Text; s.name="Text";
    // ---- colour table: the leading ';' in \colortbl defines index 0 (the "auto" colour),
    // so start EMPTY and let the parse create every entry including 0 ----
    QVector<QColor> ctab;
    { int i=rtf.indexOf("\\colortbl");
      if(i>=0){ int e=rtf.indexOf('}',i); QByteArray seg=rtf.mid(i+9,(e<0?rtf.size():e)-(i+9));
        int r=0,g=0,b=0; bool any=false;
        for(int k=0;k<seg.size();){
            if(seg[k]=='\\'){ int j=k+1; QByteArray cw; while(j<seg.size()&&isalpha((uchar)seg[j])){cw+=seg[j];++j;}
                int num=0; while(j<seg.size()&&isdigit((uchar)seg[j])){num=num*10+(seg[j]-'0');++j;}
                if(cw=="red"){r=num;any=true;} else if(cw=="green"){g=num;any=true;} else if(cw=="blue"){b=num;any=true;}
                k=j; }
            else if(seg[k]==';'){ ctab.push_back(any?QColor(r,g,b):QColor(Qt::black)); r=g=b=0; any=false; ++k; }
            else ++k;
        } } }
    // ---- main token walk ----
    const QString baseFam=s.font.family(); double baseSize=s.font.pointSizeF()>0?s.font.pointSizeF():24;
    bool bold=false, ital=false; double fs=baseSize; QColor col(Qt::black);
    QString plain; QVector<QFont> fonts; QVector<QColor> colors; QVector<QString> urls;
    int depth=0, skipDepth=-1;   // skipDepth: inside a header/ignored group
    auto mkFont=[&]{ QFont f(baseFam, -1); f.setPointSizeF(fs>0?fs:baseSize); f.setBold(bold); f.setItalic(ital); return f; };
    // Robustness: build ONE QFont per run (bold/italic/size change) and share it across the run's chars —
    // Qt implicit sharing means push_back copies only the d-pointer, so a huge RTF can't allocate a fresh
    // heavyweight QFontPrivate per character. Also cap total emitted chars so the parallel vectors stay bounded.
    static const int kMaxRtfChars=2000000;
    QFont curFont=mkFont(); bool fontDirty=false;
    auto emitCh=[&](QChar ch){ if(plain.size()>=kMaxRtfChars) return;
        if(fontDirty){ curFont=mkFont(); fontDirty=false; }
        plain+=ch; fonts.push_back(curFont); colors.push_back(col); urls.push_back(QString()); };
    const int N=rtf.size();
    for(int i=0;i<N;){
        char c=rtf[i];
        if(c=='{'){ ++depth; ++i; continue; }
        if(c=='}'){ if(skipDepth>=0 && depth<=skipDepth) skipDepth=-1; --depth; ++i; continue; }
        if(c=='\\'){
            // escapes
            if(i+1<N && (rtf[i+1]=='\\'||rtf[i+1]=='{'||rtf[i+1]=='}')){ if(skipDepth<0) emitCh(QChar(rtf[i+1])); i+=2; continue; }
            if(i+1<N && rtf[i+1]=='\''){ // \'hh hex byte
                int hv=0; QByteArray hx=rtf.mid(i+2,2); bool ok=false; hv=hx.toInt(&ok,16);
                if(ok && skipDepth<0) emitCh(QChar(hv)); i+=4; continue; }
            if(i+1<N && rtf[i+1]=='*'){ if(skipDepth<0) skipDepth=depth; i+=2; continue; }
            // control word
            int j=i+1; QByteArray cw; while(j<N && isalpha((uchar)rtf[j])){cw+=rtf[j];++j;}
            int sign=1; if(j<N&&rtf[j]=='-'){sign=-1;++j;} int num=0; bool hasNum=false;
            while(j<N&&isdigit((uchar)rtf[j])){hasNum=true;num=num*10+(rtf[j]-'0');++j;}
            num*=sign;
            if(j<N && rtf[j]==' ') ++j;   // a single trailing space is a delimiter, not text
            if(cw=="fonttbl"||cw=="colortbl"||cw=="stylesheet"||cw=="info"||cw=="pict"||cw=="header"||cw=="footer"){ skipDepth=depth; }
            else if(skipDepth<0){
                if(cw=="b"){ bold=!hasNum||num!=0; fontDirty=true; }
                else if(cw=="i"){ ital=!hasNum||num!=0; fontDirty=true; }
                else if(cw=="fs"){ fs=(hasNum?num:baseSize*2)/2.0; fontDirty=true; }
                else if(cw=="cf"){ int idx=hasNum?num:0; col=(idx>=0&&idx<ctab.size())?ctab[idx]:QColor(Qt::black); }
                else if(cw=="par"||cw=="line") emitCh(QChar('\n'));
                else if(cw=="plain"){ bold=false; ital=false; fs=baseSize; col=QColor(Qt::black); fontDirty=true; }
                else if(cw=="tab") emitCh(QChar('\t'));
            }
            i=j; continue;
        }
        if(c=='\r'||c=='\n'){ ++i; continue; }   // literal line breaks in RTF source are ignored
        if(skipDepth<0) emitCh(QChar((uchar)c));
        ++i;
    }
    if(plain.isEmpty()) return s;
    s.text=plain; s.font=fonts.isEmpty()?s.font:fonts.first();
    s.fill.type=FillType::Solid; s.fill.color=colors.isEmpty()?QColor(Qt::black):colors.first();
    s.textRuns=coalesceRuns(fonts,colors,urls);
    return s;
}

// Clipboard → styled Text shape. Prefers HTML, then RTF; returns false when neither is
// present so the caller can fall back to the internal shape-clipboard / plain paste.
bool Canvas::pasteRichText(){
    const QMimeData* md=QGuiApplication::clipboard()->mimeData();
    if(!md) return false;
    Shape s;
    if(md->hasHtml()) s=shapeFromHtml(md->html());
    else if(md->hasFormat("text/rtf")) s=shapeFromRtf(md->data("text/rtf"));
    else if(md->hasFormat("application/rtf")) s=shapeFromRtf(md->data("application/rtf"));
    else if(md->hasFormat("text/richtext")) s=shapeFromRtf(md->data("text/richtext"));
    else return false;
    if(s.text.isEmpty()) return false;
    pushUndo("Paste rich text");
    s.transform=QTransform::fromTranslate(m_doc.pageSize.width()*0.12, m_doc.pageSize.height()*0.15);
    m_doc.shapes.push_back(s);
    m_sel={ (int)m_doc.shapes.size()-1 };
    emit documentChanged(); emit selectionChanged(); update();
    return true;
}

// Recompute each text frame's repel obstacles: LOCAL-space rects of every `repel`-marked
// object that overlaps the frame. Derived cache (not undone/serialised); run before render
// so dragging a repelling object reflows the surrounding text live. Empty when nothing repels
// → the frame layout is byte-identical to before repel existed.
void Canvas::reflowText(){
    QVector<QRectF> obst;   // doc-space bounds of every repelling object
    std::function<void(const Shape&)> gather=[&](const Shape& s){ if(s.repel) obst.push_back(s.docBounds()); for(const Shape& c:s.children) gather(c); };
    for(const Shape& s: m_doc.shapes) gather(s);
    std::function<void(Shape&)> apply=[&](Shape& s){
        if(s.type==ShapeType::Text && s.textFrame){
            QVector<QRectF> loc;
            if(!obst.isEmpty()){ const QRectF fdoc=s.docBounds(); const QTransform inv=s.transform.inverted();
                for(const QRectF& o: obst){ if(o==s.docBounds()) continue; if(!o.intersects(fdoc)) continue; loc.push_back(inv.mapRect(o)); } }
            if(loc!=s.textWrapObstacles) s.textWrapObstacles=loc;
        }
        for(Shape& c: s.children) apply(c);
    };
    for(Shape& s: m_doc.shapes) apply(s);

    // Inline anchored graphics (T6): reserve a gap in each frame for every graphic anchored to
    // it, then reposition the graphic to sit in the gap the layout produced. Top-level only.
    for(Shape& s: m_doc.shapes) if(s.type==ShapeType::Text && s.textFrame) s.textInlineBoxes.clear();
    QVector<int> anchored;
    for(int gi=0; gi<m_doc.shapes.size(); ++gi){ const Shape& g=m_doc.shapes[gi];
        if(g.anchorTextIndex>=0 && g.anchorTextIndex<m_doc.shapes.size() && g.anchorTextIndex!=gi
           && m_doc.shapes[g.anchorTextIndex].type==ShapeType::Text && m_doc.shapes[g.anchorTextIndex].textFrame){
            m_doc.shapes[g.anchorTextIndex].textInlineBoxes.push_back({g.anchorChar, g.docBounds().size()});
            anchored.push_back(gi); } }
    // resolve each frame's box positions once, then move the graphics into place
    for(int gi : anchored){ Shape& g=m_doc.shapes[gi]; Shape& frame=m_doc.shapes[g.anchorTextIndex];
        QVector<QPair<int,QSizeF>> sorted=frame.textInlineBoxes;
        std::sort(sorted.begin(),sorted.end(),[](const QPair<int,QSizeF>&a,const QPair<int,QSizeF>&b){return a.first<b.first;});
        QVector<QRectF> boxes; frame.layoutFrameInline(&boxes);
        // find this graphic's slot = the sorted index whose char pos matches (first unused match)
        int slot=-1; for(int k=0;k<sorted.size();++k){ if(sorted[k].first==g.anchorChar){ slot=k; break; } }
        if(slot>=0 && slot<boxes.size() && !boxes[slot].isNull()){
            // Nudge the graphic (in DOC space) so its top-left lands in the slot, PRESERVING its
            // existing scale/rotation. Composing the delta on the left keeps it stable across the
            // per-paint reflow (once seated, the delta is ~0), unlike replacing the whole transform.
            QPointF target=frame.transform.map(boxes[slot].topLeft());
            QPointF cur=g.docBounds().topLeft();
            g.transform = g.transform * QTransform::fromTranslate(target.x()-cur.x(), target.y()-cur.y()); } }
}

// Web font embedding (P6): resolve each family via fontconfig (fc-match), read the file, and
// emit an @font-face rule embedding it as base64 so a published page needs no installed fonts.
QString Canvas::fontFaceCss(const QStringList& families){
    QString css; QSet<QString> done;
    for(const QString& fam: families){ const QString f=fam.trimmed(); if(f.isEmpty()||done.contains(f)) continue; done.insert(f);
        QProcess proc; proc.start("fc-match", {"-f","%{file}", f});
        if(!proc.waitForFinished(2500)) continue;
        const QString path=QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
        if(path.isEmpty()) continue;
        QFile ff(path); if(!ff.open(QIODevice::ReadOnly)) continue; const QByteArray bytes=ff.readAll();
        if(bytes.isEmpty() || bytes.size()>4*1024*1024) continue;            // skip missing/huge faces
        const bool otf = path.endsWith(".otf",Qt::CaseInsensitive);
        css += "@font-face{font-family:'"+f.toHtmlEscaped()+"';font-display:swap;src:url(data:font/"
             + QString(otf?"otf":"ttf") + ";base64," + QString::fromLatin1(bytes.toBase64())
             + ") format('" + QString(otf?"opentype":"truetype") + "')}\n";
    }
    return css;
}

// Built-in page/website templates (W4). Each returns a ready-made Document with shapes already
// positioned + web-layout hints set, so "New from Template" + "Publish Website" produces a page.
QStringList Canvas::templateNames(){ return {"Blank", "Landing Page", "Business Card", "Photo Portfolio", "Presentation"}; }

Document Canvas::templateDocument(int kind){
    Document d;
    auto text=[&](const QString& t, double x, double y, int pt, QColor c, bool frame=false, double w=0, int align=0){
        Shape s; s.type=ShapeType::Text; s.text=t; s.font=QFont("DejaVu Sans",pt); s.fill.type=FillType::Solid; s.fill.color=c;
        s.textAlign=align; if(frame){ s.textFrame=true; s.rect=QRectF(0,0,w,400); } s.transform=QTransform::fromTranslate(x,y); return s; };
    auto rect=[&](double x,double y,double w,double h,QColor c){ Shape s; s.type=ShapeType::Rect; s.rect=QRectF(0,0,w,h);
        s.fill.type=FillType::Solid; s.fill.color=c; s.transform=QTransform::fromTranslate(x,y); return s; };
    switch(kind){
    case 1: {   // Landing page: sticky nav bar, hero heading, subhead, CTA button, content
        d.pageSize=QSizeF(1200,1600); d.breakpoints={480,768};
        Shape nav=rect(0,0,1200,64,QColor(28,32,44)); nav.webStretch=1; nav.webSticky=true; d.addShape(nav);
        Shape brand=text("MySite", 40, 18, 20, Qt::white); brand.webSticky=true; d.addShape(brand);
        Shape hero=rect(0,64,1200,420,QColor(60,140,240)); hero.webStretch=1; d.addShape(hero);
        Shape h1=text("Build something great", 0, 190, 54, Qt::white, true, 1200, 1); h1.webStretch=1; d.addShape(h1);
        Shape sub=text("A responsive one-page site, published straight from XarQ.", 0, 300, 22, QColor(235,240,255), true, 1200, 1); sub.webStretch=1; d.addShape(sub);
        Shape btn=rect(500,360,200,56,QColor(255,196,0)); d.addShape(btn);
        Shape btnT=text("Get started", 540, 376, 20, QColor(30,30,30)); btnT.url="#contact"; d.addShape(btnT);
        d.addShape(text("Features", 60, 540, 34, QColor(20,20,20)));
        d.addShape(text("Fast. Responsive. Beautiful. Everything you design flows to the web with breakpoints, "
                        "stretchy layout, and sticky navigation — no code required.", 60, 600, 20, QColor(60,60,60), true, 1080, 0));
        break; }
    case 2: {   // Business card (88x55mm ~ 350x210 px @ ~100dpi)
        d.pageSize=QSizeF(350,210);
        d.addShape(rect(0,0,350,210,QColor(245,247,250)));
        d.addShape(rect(0,0,10,210,QColor(60,140,240)));
        d.addShape(text("Alex Designer", 28, 40, 24, QColor(20,20,20)));
        d.addShape(text("Creative Director", 28, 78, 13, QColor(90,90,90)));
        d.addShape(text("alex@example.com", 28, 130, 12, QColor(60,140,240)));
        d.addShape(text("+1 555 0100", 28, 150, 12, QColor(60,60,60)));
        d.addShape(text("www.example.com", 28, 170, 12, QColor(60,60,60)));
        break; }
    case 3: {   // Photo portfolio: title + a grid of placeholder tiles
        d.pageSize=QSizeF(1000,1200); d.breakpoints={480,768};
        d.addShape(text("Portfolio", 40, 30, 44, QColor(20,20,20)));
        const QColor tints[6]={QColor(120,170,240),QColor(240,170,120),QColor(150,210,160),QColor(220,150,200),QColor(160,200,220),QColor(230,200,140)};
        for(int r=0;r<3;++r) for(int col=0;col<2;++col){ int i=r*2+col;
            Shape tile=rect(40+col*470, 120+r*360, 450, 330, tints[i]); tile.cornerRadius=8; d.addShape(tile);
            d.addShape(text(QString("Project %1").arg(i+1), 60+col*470, 130+r*360, 18, Qt::white)); }
        break; }
    case 4: {   // Presentation — a 16:9 title slide (1920×1080)
        d.pageSize=QSizeF(1920,1080);
        d.addShape(rect(0,0,1920,1080,QColor(24,27,34)));                 // dark slide background
        d.addShape(rect(140,596,120,6,QColor(45,111,247)));               // accent rule under the title
        d.addShape(text("Presentation title", 136, 430, 92, Qt::white, true, 1648, 0));
        d.addShape(text("Subtitle · your name · date", 140, 636, 32, QColor(150,160,175), true, 1648, 0));
        break; }
    default: break;   // Blank
    }
    return d;
}

// Anchor the selected graphic inline to the first text frame at charPos: it then flows with the
// text (reflow repositions it). Returns false if there's no graphic selected or no text frame.
bool Canvas::anchorSelectionToText(int charPos){
    if(m_sel.isEmpty()) return false;
    int frameIdx=-1; for(int i=0;i<m_doc.shapes.size();++i) if(m_doc.shapes[i].type==ShapeType::Text && m_doc.shapes[i].textFrame){ frameIdx=i; break; }
    if(frameIdx<0){ emit statusMessage("Create a text frame (area) first to anchor a graphic into."); return false; }
    int gi=m_sel.first(); if(gi==frameIdx || m_doc.shapes[gi].type==ShapeType::Text) { emit statusMessage("Select the graphic (not the text) to anchor."); return false; }
    pushUndo("Anchor graphic to text");
    m_doc.shapes[gi].anchorTextIndex=frameIdx;
    m_doc.shapes[gi].anchorChar=qBound(0,charPos,m_doc.shapes[frameIdx].text.size());
    reflowText();
    emit documentChanged(); emit selectionChanged(); update();
    return true;
}

// Non-destructive photo adjust (Gap 4): re-bake the selected bitmap from its unmodified source
// with the given parameters, and store the parameters so the Adjust dialog can restore its sliders.
// Bake a photo's non-destructive adjustment from its original: apply the adjust params, then (Photo Mask
// Painter) where the mask is <255 blend the ORIGINAL back in so the adjustment is scoped to the painted
// region, then crop. Pure — no undo/selection side effects. Used by applyPhotoAdjust + setPhotoMaskFromRegion.
static QImage bakeAdjustedPhoto(const Fill& fill, const Fill::Adjust& a){
    QImage adj=adjustPhoto(fill.origImage, a.brightness,a.contrast,a.saturation, a.gray, false, a.redeye,
                           a.blur,a.sharpen,a.vignette, a.lens,a.hdr, a.blackPt,a.whitePt,a.gamma);
    if(a.temp!=0 || a.tint!=0){                        // white balance: warm/cool (R↑B↓) + magenta/green (G∓)
        adj=adj.convertToFormat(QImage::Format_ARGB32);
        const int dr=a.temp*45/100, db=-a.temp*45/100, dg=-a.tint*45/100;
        for(int y=0;y<adj.height();++y){ QRgb* p=reinterpret_cast<QRgb*>(adj.scanLine(y));
            for(int x=0;x<adj.width();++x){ QRgb c=p[x];
                p[x]=qRgba(qBound(0,qRed(c)+dr,255), qBound(0,qGreen(c)+dg,255), qBound(0,qBlue(c)+db,255), qAlpha(c)); } } }
    const int W=adj.width(), H=adj.height();
    if(!fill.adjustMask.isNull() && adj.size()==fill.origImage.size()){
        QImage msk=fill.adjustMask.scaled(W,H,Qt::IgnoreAspectRatio,Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
        QImage orig=fill.origImage.convertToFormat(QImage::Format_ARGB32);
        adj=adj.convertToFormat(QImage::Format_ARGB32);
        for(int y=0;y<H;++y){ const uchar* mp=msk.constScanLine(y);
            const QRgb* op=reinterpret_cast<const QRgb*>(orig.constScanLine(y));
            QRgb* ap=reinterpret_cast<QRgb*>(adj.scanLine(y));
            for(int x=0;x<W;++x){ int m=mp[x]; if(m==255) continue; int im=255-m;
                ap[x]=qRgba((qRed(ap[x])*m+qRed(op[x])*im)/255, (qGreen(ap[x])*m+qGreen(op[x])*im)/255,
                            (qBlue(ap[x])*m+qBlue(op[x])*im)/255, (qAlpha(ap[x])*m+qAlpha(op[x])*im)/255); } } }
    QRect crop(a.cropL*W/100, a.cropT*H/100, qMax(1,W-(a.cropL+a.cropR)*W/100), qMax(1,H-(a.cropT+a.cropB)*H/100));
    if(crop!=QRect(0,0,W,H)) adj=adj.copy(crop);
    return adj;
}
void Canvas::applyPhotoAdjust(const Fill::Adjust& a){
    applyToSelection([&](Shape& s){ if(s.fill.type!=FillType::Bitmap) return;
        if(s.fill.origImage.isNull()) s.fill.origImage=s.fill.image;
        s.fill.image=bakeAdjustedPhoto(s.fill,a); s.fill.adjust=a;
    }, "Adjust photo");
}

// Photo Mask Painter: rasterize the selected photo's painted region (regionPath, from the Region Painter /
// Magnetic Lasso tools) into an adjustment mask, so its non-destructive adjustments only affect that area.
void Canvas::setPhotoMaskFromRegion(int si){
    if(si<0||si>=m_doc.shapes.size()) return;
    Shape& s=m_doc.shapes[si];
    if(s.fill.type!=FillType::Bitmap){ emit statusMessage("Select a photo to mask."); return; }
    if(s.regionPath.isEmpty()){ emit statusMessage("Paint a region first (Region Painter or Magnetic Lasso), then scope the adjustment to it."); return; }
    if(s.fill.origImage.isNull()) s.fill.origImage=s.fill.image;
    const int W=s.fill.origImage.width(), H=s.fill.origImage.height(); if(W<1||H<1) return;
    QRectF r=s.rect; if(r.width()<1||r.height()<1) r=s.regionPath.boundingRect(); if(r.width()<1||r.height()<1) return;
    QImage mask(W,H,QImage::Format_Grayscale8); mask.fill(0);
    { QPainter g(&mask); g.setRenderHint(QPainter::Antialiasing);
      g.scale(W/r.width(), H/r.height()); g.translate(-r.left(),-r.top());
      g.setPen(Qt::NoPen); g.setBrush(Qt::white); g.drawPath(s.regionPath); }
    pushUndo("Adjustment mask");
    s.fill.adjustMask=mask;
    s.fill.image=bakeAdjustedPhoto(s.fill, s.fill.adjust);   // re-bake so the scoping shows immediately
    emit documentChanged(); update();
    emit statusMessage("Adjustments scoped to the painted region — tweak Brightness/Levels to see it apply there only.");
}
void Canvas::clearPhotoMask(int si){
    if(si<0||si>=m_doc.shapes.size()) return;
    Shape& s=m_doc.shapes[si]; if(s.fill.adjustMask.isNull()) return;
    pushUndo("Clear adjustment mask");
    s.fill.adjustMask=QImage();
    if(s.fill.type==FillType::Bitmap && !s.fill.origImage.isNull()) s.fill.image=bakeAdjustedPhoto(s.fill, s.fill.adjust);
    emit documentChanged(); update();
}
bool Canvas::hasPhotoMask(int si) const {
    return si>=0 && si<m_doc.shapes.size() && !m_doc.shapes[si].fill.adjustMask.isNull();
}

// Add a named spot/Pantone ink to the document palette; it gets its own separation plate.
int Canvas::addSpotColor(const QString& name, const QColor& c){
    pushUndo("Add spot colour");
    NamedColor nc; nc.name=name; nc.color=c; nc.spot=true; nc.theme=true; m_doc.namedColors.push_back(nc);
    emit documentChanged(); update(); return m_doc.namedColors.size()-1;
}

// Extract the selected photo's colour palette and append it to the document's theme colours.
int Canvas::addPaletteFromSelectedPhoto(int n){
    Shape* s=firstSelected();
    if(!s || s->fill.type!=FillType::Bitmap || s->fill.image.isNull()){ emit statusMessage("Select a photo/bitmap to extract colours from."); return 0; }
    const QVector<QColor> pal = extractPalette(s->fill.image, n);
    if(pal.isEmpty()) return 0;
    pushUndo("Extract colours");
    const int base=m_doc.namedColors.size();
    for(int i=0;i<pal.size();++i){ NamedColor nc; nc.name=QString("Photo %1").arg(base+i+1); nc.color=pal[i]; nc.theme=true; m_doc.namedColors.push_back(nc); }
    emit statusMessage(QString("Added %1 colours from the photo to the document palette.").arg(pal.size()));
    emit documentChanged(); emit selectionChanged(); update();
    return pal.size();
}

// Roughen: resample each subpath and displace every sample by a DETERMINISTIC hash-based offset
// (magnitude ≤ amount) → a rough / hand-drawn outline. Deterministic so it's stable & testable.
void Canvas::roughenSelection(double amount, double density){
    if (m_sel.isEmpty() || amount<=0) return;
    pushUndo("Roughen");
    const double step = qMax(1.0, density);
    auto h=[](quint32 k){ k^=k>>16; k*=0x7feb352du; k^=k>>15; k*=0x846ca68bu; k^=k>>16; return (k & 0xffffffu)/double(0xffffffu); };
    for (int i : m_sel){ if(i<0||i>=m_doc.shapes.size()) continue; Shape& s=m_doc.shapes[i];
        if (s.type==ShapeType::Group) continue;
        const QPainterPath src=s.localPath(); if(src.isEmpty()) continue;
        QPainterPath out; quint32 seed=1;
        auto jit=[&](QPointF p){ QPointF j(p.x()+(h(seed)*2-1)*amount, p.y()+(h(seed+7919u)*2-1)*amount); ++seed; return j; };
        for (const QPolygonF& poly : src.toSubpathPolygons()){
            if (poly.size()<2) continue;
            const bool closed = poly.first()==poly.last();
            bool first=true;
            for (int p=0;p+1<poly.size();++p){ const QPointF a=poly[p], b=poly[p+1]; const double len=QLineF(a,b).length();
                const int n=qMax(1,int(len/step));
                for (int q=0;q<n;++q){ const QPointF pt=jit(a+(b-a)*(double(q)/n));
                    if(first){ out.moveTo(pt); first=false; } else out.lineTo(pt); } }
            if(!first){ if(closed) out.closeSubpath(); else out.lineTo(jit(poly.last())); }
        }
        if (out.elementCount()==0) continue;
        out.setFillRule(s.fillRule);
        s.type=ShapeType::Path; s.path=out;
    }
    emit documentChanged(); emit selectionChanged(); update();
}

// Outline each selected shape's stroke into an editable filled Path (Illustrator "Outline Stroke").
void Canvas::strokeToPath(){
    if (m_sel.isEmpty()) return;
    const QVector<int> sel = m_sel;                 // snapshot: we may append new shapes
    bool any=false;
    for (int i : sel) if (i>=0 && i<m_doc.shapes.size()){ const Shape& s=m_doc.shapes[i];
        if (s.type!=ShapeType::Group && s.stroke.enabled && s.stroke.width>0) { any=true; break; } }
    if (!any) return;
    pushUndo("Convert stroke to path");
    QVector<int> nsel;
    for (int i : sel){ if (i<0||i>=m_doc.shapes.size()) continue;
        Shape& s = m_doc.shapes[i];
        if (s.type==ShapeType::Group || !s.stroke.enabled || s.stroke.width<=0){ nsel.push_back(i); continue; }
        QPainterPathStroker st; st.setWidth(qMax(0.01, s.stroke.width));
        st.setCapStyle(s.stroke.cap); st.setJoinStyle(s.stroke.join);
        if (!s.stroke.dashes.isEmpty()) st.setDashPattern(s.stroke.dashes);
        QPainterPath outline = st.createStroke(s.localPath());
        if (outline.isEmpty()){ nsel.push_back(i); continue; }
        outline.setFillRule(Qt::WindingFill);
        const QColor sc = s.stroke.color;
        if (s.fill.type == FillType::None){                     // stroke-only (a line) → the shape becomes the filled outline
            s.type=ShapeType::Path; s.path=outline; s.fillRule=Qt::WindingFill;
            s.fill.type=FillType::Solid; s.fill.color=sc; s.stroke.enabled=false; s.name="stroke";
            nsel.push_back(i);
        } else {                                                // filled + stroked → keep the fill (drop its stroke), add the outline above
            Shape o; o.type=ShapeType::Path; o.path=outline; o.fillRule=Qt::WindingFill;
            o.fill.type=FillType::Solid; o.fill.color=sc; o.stroke.enabled=false;
            o.transform=s.transform; o.opacity=s.opacity; o.layer=s.layer; o.name="stroke";
            s.stroke.enabled=false;
            m_doc.shapes.push_back(o); nsel.push_back(i); nsel.push_back(m_doc.shapes.size()-1);
        }
    }
    m_sel = nsel;
    emit documentChanged(); emit selectionChanged(); update();
}

void Canvas::blendSelection(int steps){
    if(m_sel.size()<2) return; pushUndo("Blend");
    if(m_sel.size()==2){
        int ia=qMin(m_sel[0],m_sel[1]), ib=qMax(m_sel[0],m_sel[1]);
        QVector<Shape> mids = blendMorph(m_doc.shapes[ia], m_doc.shapes[ib], steps);
        for(int i=mids.size()-1;i>=0;--i) m_doc.shapes.insert(ia+1,mids[i]);
        m_sel.clear(); emit documentChanged(); emit selectionChanged(); update(); return;
    }
    // 3+ objects → a continuous blend CHAIN A→B→C… : blend each consecutive pair (in z-order) and insert its
    // in-betweens right after the earlier end. Process pairs back-to-front so the earlier indices stay valid.
    QVector<int> idx=m_sel; std::sort(idx.begin(),idx.end());
    for(int k=idx.size()-2;k>=0;--k){ const int ia=idx[k], ib=idx[k+1];
        QVector<Shape> mids = blendMorph(m_doc.shapes[ia], m_doc.shapes[ib], steps);
        for(int i=mids.size()-1;i>=0;--i) m_doc.shapes.insert(ia+1,mids[i]); }
    m_sel.clear(); emit documentChanged(); emit selectionChanged(); update();
}
// LIVE blend: wrap the two selected end objects in a Group that regenerates its in-betweens at render
// time (see paintShape's blendSteps branch). Re-editable — the step count is a live on-canvas handle.
void Canvas::makeLiveBlend(int steps){
    if(m_sel.size()<2) return; pushUndo("Blend");
    const int ia=qMin(m_sel[0],m_sel[1]), ib=qMax(m_sel[0],m_sel[1]);
    Shape g; g.type=ShapeType::Group; g.name="Blend"; g.blendSteps=qBound(1,steps,256);
    g.children.push_back(m_doc.shapes[ia]);      // end 0 (behind)
    g.children.push_back(m_doc.shapes[ib]);      // end 1 (front)
    m_doc.shapes.removeAt(ib); m_doc.shapes.removeAt(ia);
    m_doc.shapes.push_back(g); m_sel={(int)m_doc.shapes.size()-1};
    emit documentChanged(); emit selectionChanged(); update();
}
// Live blend ALONG A CURVE: like makeLiveBlend but the last-selected shape becomes the spine the
// regenerated steps are distributed along (stored in the group; re-flows live when the step count changes).
void Canvas::makeLiveBlendAlongPath(int steps){
    if(m_sel.size()<3){ makeLiveBlend(steps); return; }
    pushUndo("Blend along curve");
    const int spineIdx=m_sel.last();
    QVector<int> ends; for(int i:m_sel) if(i!=spineIdx) ends.push_back(i); std::sort(ends.begin(),ends.end());
    const int ia=ends[0], ib=ends[1];
    Shape g; g.type=ShapeType::Group; g.name="Blend"; g.blendSteps=qBound(1,steps,256);
    g.blendSpine=Document::docPath(m_doc.shapes[spineIdx]);
    g.children.push_back(m_doc.shapes[ia]); g.children.push_back(m_doc.shapes[ib]);
    QVector<int> rm={ia,ib,spineIdx}; std::sort(rm.begin(),rm.end(),std::greater<int>());
    for(int r: rm) m_doc.shapes.removeAt(r);
    m_doc.shapes.push_back(g); m_sel={(int)m_doc.shapes.size()-1};
    emit documentChanged(); emit selectionChanged(); update();
}
void Canvas::setLiveBlendSteps(int steps){
    if(m_sel.size()!=1) return;
    Shape& g=m_doc.shapes[m_sel[0]];
    if(g.type!=ShapeType::Group || g.blendSteps<=0) return;
    int ns=qBound(1,steps,256); if(ns==g.blendSteps) return;
    pushUndo("Blend steps"); g.blendSteps=ns; emit documentChanged(); emit selectionChanged(); update();
}
// The two end-object centres of a selected live blend (in view coords), for the control line + step handle.
bool Canvas::blendControlEnds(QPointF& aView, QPointF& bView) const {
    if(m_sel.size()!=1) return false;
    const Shape& g=m_doc.shapes[m_sel[0]];
    if(g.type!=ShapeType::Group || g.blendSteps<=0 || g.children.size()!=2) return false;
    auto ctr=[&](const Shape& c){ return g.transform.map(c.transform.map(c.localPath().boundingRect().center())); };
    aView=toView(ctr(g.children[0])); bView=toView(ctr(g.children[1]));
    return true;
}
// The step-count handle: offset perpendicular from the control-line midpoint (drag to change blendSteps).
bool Canvas::blendStepHandle(QPointF& outView) const {
    QPointF a,b; if(!blendControlEnds(a,b)) return false;
    const QPointF mid=(a+b)/2.0; QPointF dir=b-a; double L=std::hypot(dir.x(),dir.y());
    QPointF n = L>1e-6 ? QPointF(-dir.y()/L, dir.x()/L) : QPointF(0,-1);
    outView = mid + n*22.0;
    return true;
}
// Blend along a path: morph the two end objects (the first two selected) as usual, but place each
// in-between step along the SPINE (the last-selected shape's outline) by arc length instead of on the
// straight line between the ends. Falls back to a straight blend if fewer than 3 objects are selected.
void Canvas::blendAlongPathSelection(int steps){
    if(m_sel.size()<3){ blendSelection(steps); return; }
    pushUndo("Blend along path");
    const int spineIdx=m_sel.last();
    QVector<int> ends; for(int i:m_sel) if(i!=spineIdx) ends.push_back(i); std::sort(ends.begin(),ends.end());
    const int ia=ends[0], ib=ends[1];
    QVector<Shape> mids=blendMorph(m_doc.shapes[ia], m_doc.shapes[ib], steps);
    const QPainterPath spine=Document::docPath(m_doc.shapes[spineIdx]);
    if(spine.length()>0.01) for(int i=0;i<mids.size();++i){
        const double t=(i+1.0)/(steps+1.0);
        const QPointF target=spine.pointAtPercent(qBound(0.0,t,1.0));
        const QPointF cur=mids[i].docBounds().center();
        const QTransform tr=QTransform::fromTranslate(target.x()-cur.x(), target.y()-cur.y());
        if(mids[i].type==ShapeType::Path) mids[i].path=tr.map(mids[i].path); else mids[i].transform=mids[i].transform*tr;
    }
    for(const Shape& m: mids) m_doc.shapes.push_back(m);
    m_sel.clear(); emit documentChanged(); emit selectionChanged(); update();
}

void Canvas::initMould(int idx){
    if(idx<0||idx>=m_doc.shapes.size()) { m_mouldShape=-1; m_mouldHasSource=false; return; }
    m_mouldShape=idx; Shape& s=m_doc.shapes[idx]; QRectF lb=s.localBounds();
    if(lb.width()<1) lb.setWidth(1); if(lb.height()<1) lb.setHeight(1);
    m_mouldLocal[0]=lb.topLeft(); m_mouldLocal[1]=lb.topRight(); m_mouldLocal[2]=lb.bottomRight(); m_mouldLocal[3]=lb.bottomLeft();
    for(int i=0;i<4;++i) m_mouldCorners[i]=s.transform.map(m_mouldLocal[i]);
    // Straight starting cage: each edge's two controls sit at 1/3 and 2/3 along the chord, so the
    // envelope begins as an undistorted rectangle until a handle is dragged.
    for(int i=0;i<4;++i){ const QPointF& a=m_mouldCorners[i]; const QPointF& b=m_mouldCorners[(i+1)&3];
        m_mouldCtrl[2*i]=a+(b-a)/3.0; m_mouldCtrl[2*i+1]=a+(b-a)*2.0/3.0; }
    // Stash the pristine shape so every subsequent envelope edit re-warps from the original geometry.
    m_mouldSource=s; m_mouldHasSource=true;
}
void Canvas::setMouldMode(int mode){ m_mouldMode=(mode?1:0); if(m_mouldShape>=0) initMould(m_mouldShape); update(); }
void Canvas::moveMouldCorner(int i, const QPointF& docPt){
    if(i<0||i>3||m_mouldShape<0||m_mouldShape>=m_doc.shapes.size()) return;
    if(m_mouldMode==1){
        // Move the corner and drag its two incident edge-controls along, so a straight edge stays
        // straight when you reshape a corner (Xara's default feel).
        QPointF delta=docPt-m_mouldCorners[i];
        m_mouldCtrl[2*i]+=delta; m_mouldCtrl[2*((i+3)&3)+1]+=delta;
    }
    m_mouldCorners[i]=docPt; applyMould();
}
void Canvas::moveMouldCtrl(int i, const QPointF& docPt){
    if(i<0||i>=8||m_mouldShape<0||m_mouldShape>=m_doc.shapes.size()) return;
    m_mouldCtrl[i]=docPt; if(m_mouldMode!=1) m_mouldMode=1; bakeEnvelope();
}
void Canvas::applyMould(){
    if(m_mouldShape<0||m_mouldShape>=m_doc.shapes.size())return;
    if(m_mouldMode==1){ bakeEnvelope(); return; }
    QPolygonF from,to; for(int i=0;i<4;++i){ from<<m_mouldLocal[i]; to<<m_mouldCorners[i]; }
    QTransform t; if(QTransform::quadToQuad(from,to,t)){ m_doc.shapes[m_mouldShape].transform=t; emit documentChanged(); update(); }
}
// Build the doc-space Coons cage from the canvas handles. coonsWarp expects cor0=BL, cor1=TL,
// cor2=TR, cor3=BR (edges cor0->cor1->cor2->cor3); our handles are corners [TL,TR,BR,BL] with edge
// controls ctrl[2i],ctrl[2i+1] running corner i->i+1, so remap into the cage's order/orientation.
static Envelope buildMouldCage(const QPointF corners[4], const QPointF ctrls[8]){
    Envelope env;
    env.cor[0]=corners[3]; env.cor[1]=corners[0]; env.cor[2]=corners[1]; env.cor[3]=corners[2];
    env.ec1[0]=ctrls[6]; env.ec2[0]=ctrls[7];   // BL->TL  (our left edge,   corner3->corner0)
    env.ec1[1]=ctrls[0]; env.ec2[1]=ctrls[1];   // TL->TR  (our top edge,    corner0->corner1)
    env.ec1[2]=ctrls[2]; env.ec2[2]=ctrls[3];   // TR->BR  (our right edge,  corner1->corner2)
    env.ec1[3]=ctrls[4]; env.ec2[3]=ctrls[5];   // BR->BL  (our bottom edge, corner2->corner3)
    return env;
}
// Warp one (possibly grouped) shape through the cage into baked Path geometry. localSpace=true warps
// the shape in its OWN local frame with srcRect = its local bounds — so a rotated/sheared shape maps
// through its oriented box and an identity cage is a true identity. localSpace=false warps in doc
// space against a SHARED srcRect, so a multi-object selection keeps its relative layout as one unit.
// A solid fill is carried through; a gradient bakes to its representative colour (as the XAR reader).
static bool warpShapeThroughCage(const Shape& source, const QRectF& srcRect, const Envelope& env, bool localSpace, Shape& out){
    QVector<Shape> leaves;
    std::function<void(const Shape&,const QTransform&)> collect=[&](const Shape& s,const QTransform& acc){
        if(s.type==ShapeType::Group){ for(const Shape& ch:s.children) collect(ch, ch.transform*acc); return; }
        QPainterPath geom=acc.map(s.localPath()); if(geom.isEmpty()) return;
        QPainterPath wp=warpThroughEnvelope(geom, srcRect, env); if(wp.isEmpty()) return;
        Shape leaf; leaf.type=ShapeType::Path; leaf.name=s.name.isEmpty()?QStringLiteral("Moulded"):s.name;
        leaf.path=wp; leaf.fillRule=s.fillRule; leaf.opacity=s.opacity; leaf.transp=s.transp; leaf.stroke=s.stroke;
        leaf.fill=s.fill;
        if(leaf.fill.type!=FillType::Solid && leaf.fill.type!=FillType::None){ leaf.fill=Fill(); leaf.fill.type=FillType::Solid; leaf.fill.color=s.fill.representativeColor(); }
        leaves.push_back(leaf);
    };
    collect(source, localSpace? QTransform() : source.transform);   // local frame skips source.transform
    if(leaves.isEmpty()) return false;
    if(leaves.size()==1) out=leaves.first();
    else { out=Shape(); out.type=ShapeType::Group; out.name=source.name.isEmpty()?QStringLiteral("Moulded"):source.name; out.children=leaves; }
    return true;
}
// Warp the pristine stashed shape through the current cage and replace the moulded shape (interactive
// single-shape path; warps in the shape's own local frame so an untouched straight cage is identity).
void Canvas::bakeEnvelope(){
    if(!m_mouldHasSource || m_mouldShape<0 || m_mouldShape>=m_doc.shapes.size()) return;
    Envelope env=buildMouldCage(m_mouldCorners, m_mouldCtrl);
    QRectF src=m_mouldSource.localBounds(); if(src.width()<1||src.height()<1) return;
    Shape out; if(!warpShapeThroughCage(m_mouldSource, src, env, /*localSpace=*/true, out)) return;
    // Reject a degenerate cage that flings geometry off to (near-)infinity.
    QRectF wbb=out.docBounds(); double lim=qMax(m_doc.pageSize.width(),m_doc.pageSize.height())*40.0+10000.0;
    if(!qIsFinite(wbb.x())||!qIsFinite(wbb.y())||wbb.width()>lim||wbb.height()>lim||qAbs(wbb.x())>2*lim||qAbs(wbb.y())>2*lim) return;
    out.layer=m_doc.shapes[m_mouldShape].layer;
    m_doc.shapes[m_mouldShape]=out;
    emit documentChanged(); update();
}
// Bake a supplied cage into the whole selection. Undo-able. A single shape warps in its own local
// frame (rotation-safe); a multi-shape selection warps as ONE unit in doc space against the shared
// selection bounds, so the objects keep their relative positions/sizes within the envelope.
void Canvas::applyEnvelopeToSelection(const QPointF corners[4], const QPointF ctrls[8]){
    if(m_sel.isEmpty()) return; pushUndo("Envelope mould");
    for(int i=0;i<4;++i) m_mouldCorners[i]=corners[i];
    for(int i=0;i<8;++i) m_mouldCtrl[i]=ctrls[i];
    m_mouldMode=1;
    Envelope env=buildMouldCage(m_mouldCorners, m_mouldCtrl);
    const QVector<int> sel=m_sel;
    const double lim=qMax(m_doc.pageSize.width(),m_doc.pageSize.height())*40.0+10000.0;
    auto sane=[lim](const QRectF& r){ return qIsFinite(r.x())&&qIsFinite(r.y())&&r.width()<lim&&r.height()<lim&&qAbs(r.x())<2*lim&&qAbs(r.y())<2*lim; };
    if(sel.size()==1){
        int idx=sel.first();
        if(idx>=0 && idx<m_doc.shapes.size()){ QRectF sr=m_doc.shapes[idx].localBounds(); Shape out;
            if(sr.width()>=1 && sr.height()>=1 && warpShapeThroughCage(m_doc.shapes[idx],sr,env,true,out) && sane(out.docBounds())){
                out.layer=m_doc.shapes[idx].layer; m_doc.shapes[idx]=out; } }
    } else {
        QRectF sr; for(int idx: sel) if(idx>=0&&idx<m_doc.shapes.size()) sr=sr.united(m_doc.shapes[idx].docBounds());
        if(sr.width()>=1 && sr.height()>=1)
            for(int idx: sel){ if(idx<0||idx>=m_doc.shapes.size()) continue; Shape out;
                if(warpShapeThroughCage(m_doc.shapes[idx],sr,env,false,out) && sane(out.docBounds())){
                    out.layer=m_doc.shapes[idx].layer; m_doc.shapes[idx]=out; } }
    }
    invalidateMould();
    emit documentChanged(); emit selectionChanged(); update();
}

// ============================================================================
// In-canvas text editing — live caret + typing directly on the canvas
// ============================================================================
void Canvas::beginTextEdit(int shapeIdx, int caretIdx){
    if(shapeIdx<0 || shapeIdx>=m_doc.shapes.size() || m_doc.shapes[shapeIdx].type!=ShapeType::Text) return;
    if(m_textEdit>=0 && m_textEdit!=shapeIdx) endTextEdit();
    m_textEdit=shapeIdx; m_sel={shapeIdx};
    const int n=m_doc.shapes[shapeIdx].text.size();
    m_caret=(caretIdx<0)? n : qBound(0,caretIdx,n);
    m_caretAnchor=-1; m_caretBlink=true;
    if(!m_caretTimer){ m_caretTimer=new QTimer(this); m_caretTimer->setInterval(530);
        connect(m_caretTimer,&QTimer::timeout,this,[this]{ if(m_textEdit>=0){ m_caretBlink=!m_caretBlink; update(); } }); }
    m_caretTimer->start();
    setFocus(Qt::OtherFocusReason);
    emit selectionChanged(); update();
}
void Canvas::endTextEdit(bool keepEmpty){
    if(m_textEdit<0){ if(m_caretTimer) m_caretTimer->stop(); return; }
    int idx=m_textEdit; m_textEdit=-1; m_caretAnchor=-1;
    if(m_caretTimer) m_caretTimer->stop();
    // A freshly-created text object left empty is discarded so a stray click doesn't litter the page.
    if(m_textEditIsNew && !keepEmpty && idx>=0 && idx<m_doc.shapes.size() && m_doc.shapes[idx].text.isEmpty()){
        m_doc.shapes.removeAt(idx); m_sel.clear();
    }
    m_textEditIsNew=false;
    emit documentChanged(); emit selectionChanged(); update();
}
// Core edit: replace text[a,b) with `ins`. Undoable; drops per-character runs (the edited text becomes
// uniformly styled) so stale run ranges can't mis-format, and reflows any linked chain on next render.
void Canvas::textReplaceRange(int a, int b, const QString& ins){
    if(!editingText()) return;                       // ignore if the caret's shape vanished / isn't text
    Shape& s=m_doc.shapes[m_textEdit]; const int n=s.text.size();
    a=qBound(0,a,n); b=qBound(0,b,n); if(a>b) std::swap(a,b);
    if(a==b && ins.isEmpty()) return;
    pushUndo("Edit text");
    s.text = s.text.left(a) + ins + s.text.mid(b);
    // Editing makes the text uniformly styled and drops char-indexed metadata that would otherwise
    // point at the wrong character after the length change (runs + inline-graphic anchors).
    if(!s.textRuns.isEmpty()) s.textRuns.clear();
    if(!s.textInlineBoxes.isEmpty()) s.textInlineBoxes.clear();
    m_caret=a+ins.size(); m_caretAnchor=-1; m_caretBlink=true; if(m_caretTimer) m_caretTimer->start();
    emit documentChanged(); update();
}
// Only plain point text has exact caret maths (textCaretLocal mirrors that layout). Frames wrap
// without '\n', text-on-curve flows along a path, and tab stops jump — for those the click→index map
// would be wrong, so they fall back to the modal editor instead of an in-canvas caret.
bool Canvas::canInlineEdit(const Shape& s){
    if(s.type!=ShapeType::Text) return false;
    if(!s.textRuns.isEmpty()) return false;   // rich runs render with per-character fonts — the uniform-font
                                              // caret map would sit off the glyphs; edit via the dialog instead
    if(!s.textPath.isEmpty()) return true;    // text on a curve: the caret flows along the baseline path
                                              // (localPath's on-curve branch loops char-by-char, which the
                                              // caret map mirrors exactly — see Shape::textCaretLocal)
    if(!s.textFrame) return true;   // plain point text (with regular tabs and/or explicit tab stops)
    // Word-wrapped frame: inline-editable when its layout is the one frameVisualLines() replicates.
    // Supported: columns, indents, paragraph spacing, L/C/R align, justify (renders as left here),
    // bulleted/numbered lists (the marker sits in the gutter; text hangs at listIndent), drop caps when
    // the first character IS the cap (the common case), regular tab stops (tabWidth), explicit tab stops
    // (align/leaders/decimal — s_tabStopsX mirrors place()), hyphenation (the synthetic '-' is tracked so
    // the caret skips it), repel-around-obstacles, and inline anchored graphics (words flow with reserved
    // box gaps; the caret maps to the words), and drop caps — including leading-punctuation ones, where the
    // enlarged letter/number sits anywhere and the caret converts around it. Inline and repel are their own
    // render branches that ignore drop cap, so exclude that rare combination. Every frame layout the
    // renderer produces is now caret-mapped.
    if(!s.textInlineBoxes.isEmpty() || !s.textWrapObstacles.isEmpty()) return s.rect.width()>4 && s.dropCap<=1;
    return s.rect.width()>4;
}
// Keep the text-edit state consistent after any op that replaces/reorders shapes (undo, redo,
// setDocument, delete, reorder). If the caret's shape is gone or no longer text, silently leave edit
// mode (do NOT delete anything); otherwise clamp the caret/anchor into the possibly-shorter text.
void Canvas::resetTextEdit(){ m_textEdit=-1; m_caret=0; m_caretAnchor=-1; m_textEditIsNew=false; m_textDragging=false; if(m_caretTimer) m_caretTimer->stop(); }
void Canvas::syncTextEditToDoc(){
    if(m_textEdit<0) return;
    if(m_textEdit>=m_doc.shapes.size() || m_doc.shapes[m_textEdit].type!=ShapeType::Text){
        m_textEdit=-1; m_caretAnchor=-1; m_textEditIsNew=false; m_textDragging=false;
        if(m_caretTimer) m_caretTimer->stop(); return; }
    const int n=m_doc.shapes[m_textEdit].text.size();
    m_caret=qBound(0,m_caret,n); if(m_caretAnchor>n) m_caretAnchor=-1;
}
// While a text caret is active, claim the keys the editor uses so window/menu shortcuts (Delete,
// Ctrl+A/C/X/V) don't fire instead — accepting the ShortcutOverride routes them to keyPressEvent.
bool Canvas::event(QEvent* e){
    if((e->type()==QEvent::FocusOut || e->type()==QEvent::WindowDeactivate) && (m_radialActive || (m_radialTimer && m_radialTimer->isActive()))) radialDisarm();  // don't leave the pie stuck if focus leaves
    // A modifier+<key> chord (e.g. the Ctrl+Alt+G Soft-Group shortcut) is consumed at the app/QAction level and
    // never reaches keyPressEvent, but its ShortcutOverride IS delivered to the focused canvas first — use it to
    // dismiss the pie so a held Ctrl+Alt during such a chord can't leave the menu blooming.
    if(e->type()==QEvent::ShortcutOverride && (m_radialActive || (m_radialTimer && m_radialTimer->isActive()))){
        const int kk=static_cast<QKeyEvent*>(e)->key(); if(kk!=Qt::Key_Control && kk!=Qt::Key_Alt) radialDisarm();
    }
    if(e->type()==QEvent::ShortcutOverride && editingText()){
        auto* ke=static_cast<QKeyEvent*>(e); const int k=ke->key(); const bool ctrl=ke->modifiers()&Qt::ControlModifier;
        bool mine = k==Qt::Key_Delete||k==Qt::Key_Backspace||k==Qt::Key_Left||k==Qt::Key_Right||k==Qt::Key_Up
                 || k==Qt::Key_Down||k==Qt::Key_Home||k==Qt::Key_End||k==Qt::Key_Return||k==Qt::Key_Enter||k==Qt::Key_Escape;
        if(ctrl && (k==Qt::Key_A||k==Qt::Key_C||k==Qt::Key_X||k==Qt::Key_V)) mine=true;
        // Any plain printable keystroke (no Ctrl/Alt/Meta) is text input and MUST win over the single-key
        // tool shortcuts (T/E/L/P/B/F/V/N/M/G/Y/K…): otherwise typing "T" fired the Text-tool shortcut, which
        // switched tools and ended the edit, so in-canvas typing never worked. This mirrors keyPressEvent's gate.
        if(!(ke->modifiers()&(Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier))){
            const QString txt=ke->text(); if(!txt.isEmpty() && txt[0].isPrint()) mine=true;
        }
        if(mine){ e->accept(); return true; }   // Ctrl+Z/Y deliberately fall through to global undo/redo
    }
    return QWidget::event(e);
}
// True if a curly quote opens (vs. closes) after character `prev` (null = start of text).
static bool smartQuoteOpens(QChar prev){
    return prev.isNull() || prev.isSpace() || prev=='(' || prev=='[' || prev=='{'
        || prev==QChar(0x2018) || prev==QChar(0x201C) || prev==QChar(0x00A0);
}
void Canvas::textInsert(const QString& s){
    if(m_textEdit<0) return;
    QString ins=s;
    if(m_smartQuotes && s.size()==1 && (s[0]=='"' || s[0]=='\'')){
        const QString& t=m_doc.shapes[m_textEdit].text;
        const int at=textHasSelection()?qMin(m_caret,m_caretAnchor):m_caret;
        const QChar prev = (at>0 && at<=t.size()) ? t[at-1] : QChar();
        const bool open=smartQuoteOpens(prev);
        ins = QString(QChar(s[0]=='"' ? (open?0x201C:0x201D) : (open?0x2018:0x2019)));
    }
    if(textHasSelection()){ textReplaceRange(qMin(m_caret,m_caretAnchor), qMax(m_caret,m_caretAnchor), ins); }
    else textReplaceRange(m_caret, m_caret, ins);
}
// Char ranges [start,end) of words the spell-checker flags in `text` (interactive squiggles + tests).
QVector<QPair<int,int>> Canvas::misspelledRanges(const QString& text) const {
    QVector<QPair<int,int>> out;
    if(!Speller::instance().ready()) return out;
    static const QRegularExpression tok("[A-Za-z][A-Za-z']*");
    auto it=tok.globalMatch(text);
    while(it.hasNext()){ auto m=it.next(); const QString w=m.captured(0);
        if(w.size()<3 || w==w.toUpper() || w[0].isUpper()) continue;   // skip tiny / ALLCAPS / capitalised
        if(Speller::instance().known(w)) continue;
        out.append({m.capturedStart(0), m.capturedEnd(0)}); }
    return out;
}
// Convert straight ' " to typographic quotes across every selected text object.
void Canvas::convertSmartQuotes(){
    if(m_sel.isEmpty()) return;
    bool any=false;
    for(int i: m_sel){ if(i<0||i>=m_doc.shapes.size()||m_doc.shapes[i].type!=ShapeType::Text) continue;
        QString& t=m_doc.shapes[i].text; QString out; out.reserve(t.size());
        for(int k=0;k<t.size();++k){ QChar ch=t[k];
            if(ch=='"'||ch=='\''){ const QChar prev=k>0?t[k-1]:QChar(); const bool open=smartQuoteOpens(prev);
                out += QChar(ch=='"' ? (open?0x201C:0x201D) : (open?0x2018:0x2019)); }
            else out += ch; }
        if(out!=t){ if(!any){ pushUndo("Smart quotes"); any=true; } t=out; }
    }
    if(any){ emit documentChanged(); emit selectionChanged(); update(); }
}
void Canvas::textBackspace(){
    if(m_textEdit<0) return;
    if(textHasSelection()) textReplaceRange(qMin(m_caret,m_caretAnchor), qMax(m_caret,m_caretAnchor), QString());
    else if(m_caret>0)     textReplaceRange(m_caret-1, m_caret, QString());
}
void Canvas::textDelete(){
    if(m_textEdit<0) return;
    if(textHasSelection()) textReplaceRange(qMin(m_caret,m_caretAnchor), qMax(m_caret,m_caretAnchor), QString());
    else                   textReplaceRange(m_caret, m_caret+1, QString());
}
void Canvas::textMoveCaret(int dir, bool extend){
    if(m_textEdit<0 || m_textEdit>=m_doc.shapes.size()) return;
    const Shape& s=m_doc.shapes[m_textEdit]; const QString& t=s.text; const int n=t.size();
    if(extend && m_caretAnchor<0) m_caretAnchor=m_caret;
    int c=m_caret;
    auto lineStart=[&](int i){ int p=i; while(p>0 && t[p-1]!='\n') --p; return p; };
    auto lineEnd  =[&](int i){ int p=i; while(p<n && t[p]!='\n') ++p; return p; };
    switch(dir){
        case -1: c=qMax(0,c-1); break;
        case +1: c=qMin(n,c+1); break;
        case -2: c=lineStart(c); break;
        case +2: c=lineEnd(c);   break;
        case -3: case +3: {                         // up/down: keep the horizontal position
            QPointF cp=s.textCaretLocal(c); double dy=(dir==-3?-1:+1)*s.textLineH();
            c=qBound(0,s.textIndexAtLocal(QPointF(cp.x(), cp.y()+dy)),n); break; }
    }
    m_caret=c; if(!extend) m_caretAnchor=-1;
    m_caretBlink=true; if(m_caretTimer) m_caretTimer->start(); update();
}
void Canvas::textSelectAll(){
    if(m_textEdit<0 || m_textEdit>=m_doc.shapes.size()) return;
    m_caretAnchor=0; m_caret=m_doc.shapes[m_textEdit].text.size(); m_caretBlink=true; update();
}
void Canvas::setCaretAtDocPoint(const QPointF& docPt, bool extend){
    if(m_textEdit<0 || m_textEdit>=m_doc.shapes.size()) return;
    const Shape& s=m_doc.shapes[m_textEdit];
    const QPointF local=s.transform.inverted().map(docPt);
    if(extend && m_caretAnchor<0) m_caretAnchor=m_caret;
    m_caret=s.textIndexAtLocal(local); if(!extend) m_caretAnchor=-1;
    m_caretBlink=true; if(m_caretTimer) m_caretTimer->start(); update();
}

// Evenly space the selection's centres between the two extremes along one axis.
void Canvas::distributeSelection(bool horizontal, bool equalGap){
    if(m_sel.size()<3)return; pushUndo("Distribute");
    QVector<int> idx=m_sel;
    // Sort by leading edge so equal-gap accounts for the actual object order along the axis.
    std::sort(idx.begin(),idx.end(),[&](int a,int b){ QRectF ba=m_doc.shapes[a].docBounds(), bb=m_doc.shapes[b].docBounds();
        return horizontal? ba.left()<bb.left() : ba.top()<bb.top(); });
    if(equalGap){
        // Equal GAP between edges: keep the first & last where they are, and give every in-between object an
        // identical clear space to its neighbour (accounts for differing object sizes, unlike equal-centre).
        const QRectF fb=m_doc.shapes[idx.first()].docBounds(), lb=m_doc.shapes[idx.last()].docBounds();
        double span = horizontal? (lb.right()-fb.left()) : (lb.bottom()-fb.top());
        double sumExtent=0; for(int i:idx) sumExtent += horizontal? m_doc.shapes[i].docBounds().width() : m_doc.shapes[i].docBounds().height();
        const double gap=(span-sumExtent)/(idx.size()-1);
        double cursor = horizontal? fb.left() : fb.top();
        for(int k=0;k<idx.size();++k){ const QRectF b=m_doc.shapes[idx[k]].docBounds();
            const double lead = horizontal? b.left() : b.top();
            if(k>0 && k<idx.size()-1){ const double dd=cursor-lead;
                m_doc.shapes[idx[k]].transform *= QTransform::fromTranslate(horizontal?dd:0, horizontal?0:dd); }
            cursor += (horizontal? b.width() : b.height()) + gap;
        }
    } else {
        double first=horizontal? m_doc.shapes[idx.first()].docBounds().center().x() : m_doc.shapes[idx.first()].docBounds().center().y();
        double last =horizontal? m_doc.shapes[idx.last()].docBounds().center().x()  : m_doc.shapes[idx.last()].docBounds().center().y();
        double step=(last-first)/(idx.size()-1);
        for(int k=1;k<idx.size()-1;++k){ QPointF c=m_doc.shapes[idx[k]].docBounds().center(); double target=first+step*k;
            QPointF dd = horizontal? QPointF(target-c.x(),0) : QPointF(0,target-c.y());
            m_doc.shapes[idx[k]].transform *= QTransform::fromTranslate(dd.x(),dd.y()); }
    }
    emit documentChanged(); update();
}
// Flip / rotate the whole selection about its combined bounding-box centre.
void Canvas::flipSelection(bool horizontal){
    if(m_sel.isEmpty())return; pushUndo("Flip"); QPointF c=selBoundsDoc().center();
    QTransform m; m.translate(c.x(),c.y()); m.scale(horizontal?-1:1, horizontal?1:-1); m.translate(-c.x(),-c.y());
    for(int i:m_sel) m_doc.shapes[i].transform *= m;
    emit documentChanged(); emit selectionChanged(); update();
}
void Canvas::rotateSelection90(bool cw){
    if(m_sel.isEmpty())return; pushUndo("Rotate 90"); QPointF c=selBoundsDoc().center();
    QTransform m; m.translate(c.x(),c.y()); m.rotate(cw?90:-90); m.translate(-c.x(),-c.y());
    for(int i:m_sel) m_doc.shapes[i].transform *= m;
    emit documentChanged(); emit selectionChanged(); update();
}

double Canvas::selectionRotation() const {
    QTransform t;
    if (inGroup()) { const Shape* c=enteredContainer(); if(!c||m_subSel.isEmpty()||m_subSel.first()>=c->children.size()) return 0.0;
                     t = containerXf() * c->children[m_subSel.first()].transform; }
    else { if(m_sel.isEmpty()) return 0.0; t = m_doc.shapes[m_sel.first()].transform; }
    double a = std::atan2(t.m12(), t.m11()) * 180.0/M_PI;
    return a < 0 ? a + 360.0 : a;
}
// A doc-space transform t applied on the RIGHT to a shape's own transform (transform*=t) moves it by t in
// doc space — and because a child's doc transform is containerXf*child.transform, editing child.transform the
// same way yields the identical doc-space delta. So these setters work through selTargets() with no per-context math.
void Canvas::setSelectionPos(double x, double y){ QRectF b=selBoundsDoc(); if(b.isNull())return; auto tg=selTargets(); if(tg.isEmpty())return; pushUndo("Move");
    QTransform t; t.translate(x-b.left(), y-b.top());
    for(Shape* s:tg) s->transform *= t;
    emit documentChanged(); emit selectionChanged(); update(); }
void Canvas::setSelectionSize(double w, double h, bool keepAspect){ QRectF b=selBoundsDoc(); auto tg=selTargets(); if(tg.isEmpty())return;
    if(b.width()<1e-3||b.height()<1e-3||w<1e-3||h<1e-3)return; pushUndo("Resize");
    double sx=w/b.width(), sy=h/b.height(); if(keepAspect){ double s=qMin(sx,sy); sx=sy=s; }
    QTransform m; m.translate(b.left(),b.top()); m.scale(sx,sy); m.translate(-b.left(),-b.top());
    const double gs=std::sqrt(qMax(1e-9,std::abs(sx*sy)));
    for(Shape* s:tg){ s->transform *= m; if(m_doc.scaleLineWidths) s->stroke.width *= gs; }   // scale line widths on numeric resize too
    emit documentChanged(); emit selectionChanged(); update(); }
void Canvas::rotateSelection(double deg){ if(qAbs(deg)<1e-3)return; auto tg=selTargets(); if(tg.isEmpty())return; pushUndo("Rotate"); QPointF c=selBoundsDoc().center();
    QTransform m; m.translate(c.x(),c.y()); m.rotate(deg); m.translate(-c.x(),-c.y());
    for(Shape* s:tg) s->transform *= m;
    emit documentChanged(); emit selectionChanged(); update(); }

void Canvas::alignSelection(int mode){ if(m_sel.size()<2)return; pushUndo("Align"); QRectF b=selBoundsDoc();
    for(int i:m_sel){ QRectF db=m_doc.shapes[i].docBounds(); double dx=0,dy=0;
        switch(mode){case 0:dx=b.left()-db.left();break;case 1:dx=b.center().x()-db.center().x();break;case 2:dx=b.right()-db.right();break;
        case 3:dy=b.top()-db.top();break;case 4:dy=b.center().y()-db.center().y();break;case 5:dy=b.bottom()-db.bottom();break;}
        QTransform t; t.translate(dx,dy); m_doc.shapes[i].transform=m_doc.shapes[i].transform*t; }
    emit documentChanged(); update(); }

void Canvas::alignSelectionAdv(int hMode,int vMode,bool toPage){
    if(m_sel.isEmpty())return; pushUndo("Align");
    QRectF ref = toPage ? QRectF(QPointF(0,0),m_doc.pageSize) : selBoundsDoc();
    for(int i:m_sel){ QRectF db=m_doc.shapes[i].docBounds(); double dx=0,dy=0;
        if(hMode==1)dx=ref.left()-db.left(); else if(hMode==2)dx=ref.center().x()-db.center().x(); else if(hMode==3)dx=ref.right()-db.right();
        if(vMode==1)dy=ref.top()-db.top(); else if(vMode==2)dy=ref.center().y()-db.center().y(); else if(vMode==3)dy=ref.bottom()-db.bottom();
        QTransform t; t.translate(dx,dy); m_doc.shapes[i].transform=m_doc.shapes[i].transform*t; }
    emit documentChanged(); update(); }

void Canvas::repeatSelection(int cols,int rows,double dx,double dy){
    if(m_sel.isEmpty() || (cols<=1 && rows<=1)) return; pushUndo("Repeat objects");
    QVector<Shape> orig; for(int i:m_sel) orig.push_back(m_doc.shapes[i]);
    QVector<int> nsel=m_sel;
    for(int r=0;r<rows;++r) for(int c=0;c<cols;++c){ if(r==0 && c==0) continue;
        QTransform off=QTransform::fromTranslate(c*dx, r*dy);
        for(const Shape& s: orig){ Shape ns=s; ns.transform=s.transform*off; nsel.push_back(m_doc.addShape(ns)); } }
    m_sel=nsel; emit documentChanged(); emit selectionChanged(); update(); }

// ---- multi-page ------------------------------------------------------------
int Canvas::currentPage() const {
    double cy = toDoc(QPointF(width()/2.0, height()/2.0)).y();
    double adv = m_doc.pageSize.height()+m_doc.pageGap;
    return qBound(0, int(cy/adv + 0.5), m_doc.pageCount-1);
}
void Canvas::addPage(){ pushUndo("Add page"); m_doc.pageCount++; emit documentChanged(); gotoPage(m_doc.pageCount-1); }
void Canvas::duplicatePage(int i){ if(i<0||i>=m_doc.pageCount)return; pushUndo("Duplicate page");
    double adv=m_doc.pageSize.height()+m_doc.pageGap; QRectF pr=m_doc.pageRect(i);
    QVector<Shape> copies;   // copy shapes whose centre is on page i, shifted to the new (inserted-after) page
    for(const Shape& s: m_doc.shapes){ QRectF b=s.docBounds(); if(b.center().y()>=pr.top() && b.center().y()<pr.bottom()){ Shape ns=s; ns.transform=s.transform*QTransform::fromTranslate(0,adv); copies.push_back(ns); } }
    // shift everything below page i down by one page to make room
    for(auto& s: m_doc.shapes){ QRectF b=s.docBounds(); if(b.center().y()>=pr.bottom()) s.transform=s.transform*QTransform::fromTranslate(0,adv); }
    for(auto& ns: copies) m_doc.shapes.push_back(ns);
    m_doc.pageCount++; emit documentChanged(); gotoPage(i+1); }
void Canvas::deletePage(int i){ if(i<0||i>=m_doc.pageCount||m_doc.pageCount<=1)return; pushUndo("Delete page");
    double adv=m_doc.pageSize.height()+m_doc.pageGap; QRectF pr=m_doc.pageRect(i);
    for(int k=m_doc.shapes.size()-1;k>=0;--k){ QRectF b=m_doc.shapes[k].docBounds();
        if(b.center().y()>=pr.top() && b.center().y()<pr.bottom()) m_doc.shapes.remove(k);
        else if(b.center().y()>=pr.bottom()) m_doc.shapes[k].transform=m_doc.shapes[k].transform*QTransform::fromTranslate(0,-adv); }
    m_doc.pageCount--; m_sel.clear(); emit documentChanged(); emit selectionChanged(); gotoPage(qMin(i,m_doc.pageCount-1)); }
void Canvas::gotoPage(int i){ i=qBound(0,i,m_doc.pageCount-1); QRectF pr=m_doc.pageRect(i);
    m_origin.setY(34 - pr.top()*m_zoom); m_origin.setX(width()/2.0 - pr.center().x()*m_zoom); update(); emit documentChanged(); }
// Reorder pages: swap the content of page i with the adjacent page i+delta (delta = ±1). Objects are
// assigned to a page band by their bounding-box centre, so the swap just translates each band's objects.
void Canvas::movePage(int i, int delta){
    const int j=i+delta;
    if(i<0||i>=m_doc.pageCount||j<0||j>=m_doc.pageCount) return;
    pushUndo("Move page");
    const double adv=m_doc.pageSize.height()+m_doc.pageGap;
    const QRectF pi=m_doc.pageRect(i), pj=m_doc.pageRect(j);
    const double dy=(j-i)*adv;
    for(Shape& s : m_doc.shapes){ const double cy=s.docBounds().center().y();   // read BEFORE moving; single pass
        if(cy>=pi.top() && cy<pi.bottom())      s.transform *= QTransform::fromTranslate(0, dy);
        else if(cy>=pj.top() && cy<pj.bottom()) s.transform *= QTransform::fromTranslate(0,-dy); }
    m_sel.clear(); emit documentChanged(); emit selectionChanged(); gotoPage(j);
}
// Ruler origin (zero-point): user-settable so coordinates read relative to a chosen document point.
void Canvas::setRulerOrigin(const QPointF& docPt){ pushUndo("Ruler origin"); m_doc.rulerOrigin=docPt; emit documentChanged(); update(); }
void Canvas::resetRulerOrigin(){ if(m_doc.rulerOrigin.isNull())return; pushUndo("Ruler origin"); m_doc.rulerOrigin=QPointF(0,0); emit documentChanged(); update(); }

// ---- view ------------------------------------------------------------------
// Arm the interactive bitmap-preview zoom + a debounce timer that does ONE crisp re-render once the gesture
// settles. While armed, paintEvent blits the last rendered backdrop scaled to the current view instead of
// re-rendering the document (instant). Adaptive: only engages when the previous full-quality frame was actually
// slow (>40 ms ≈ under 25 fps) AND a usable cache exists — on light docs the crisp render is already instant, so
// we skip the preview and never show a pixelated "pop".
void Canvas::noteZoomPanInteraction(){
    if (m_lastBackdropMs < 40.0 || m_screenCache.isNull()) return;
    m_previewZoom = true;   // preview source is always m_screenCache @ m_cacheKey.xf (kept in sync by the cache rebuild)
    if(!m_zoomSettleTimer){
        m_zoomSettleTimer = new QTimer(this); m_zoomSettleTimer->setSingleShot(true);
        connect(m_zoomSettleTimer, &QTimer::timeout, this, [this]{
            if(!m_previewZoom) return;
            m_previewZoom = false; m_cacheDirty = true; update();   // one crisp, full-quality frame after motion stops
        });
    }
    m_zoomSettleTimer->start(150);
}
void Canvas::zoomTo(double z){ noteZoomPanInteraction(); z=qBound(0.05,z,40.0); QPointF c=toDoc(QPointF(width()/2.0,height()/2.0)); m_zoom=z; QPointF nc=toView(c); m_origin+=QPointF(width()/2.0,height()/2.0)-nc; emit zoomChanged(z); update(); }
void Canvas::zoomIn(){ zoomTo(m_zoom*1.25); }
void Canvas::zoomOut(){ zoomTo(m_zoom/1.25); }
void Canvas::zoomReset(){ m_zoom=1.0; m_origin=QPointF(60,60); emit zoomChanged(1.0); update(); }
// Fit a doc-space rectangle to the view (marquee zoom / zoom-to-selection/drawing). Saves the prior view
// so Previous Zoom can bounce back. view = origin + doc*zoom.
void Canvas::zoomToRect(const QRectF& docR){
    if(docR.width()<1e-3||docR.height()<1e-3||width()<10||height()<10) return;
    m_prevZoom=m_zoom; m_prevOrigin=m_origin; m_hasPrevZoom=true;
    const double m=0.06;
    const double z=qBound(0.05, qMin(width()*(1-2*m)/docR.width(), height()*(1-2*m)/docR.height()), 40.0);
    m_zoom=z; m_origin=QPointF(width()/2.0,height()/2.0) - docR.center()*z;
    emit zoomChanged(z); update();
}
void Canvas::zoomToSelection(){ QRectF b=selBoundsDoc(); if(b.isNull()){ emit statusMessage("Select something to zoom to."); return; } zoomToRect(b.adjusted(-8,-8,8,8)); }
void Canvas::zoomToDrawing(){ QRectF b=m_doc.contentBounds(); if(b.isNull()) b=m_doc.pageRect(0); zoomToRect(b.adjusted(-8,-8,8,8)); }
void Canvas::previousZoom(){ if(!m_hasPrevZoom) return; std::swap(m_zoom,m_prevZoom); std::swap(m_origin,m_prevOrigin); emit zoomChanged(m_zoom); update(); }
void Canvas::zoomFit(){ QRectF b=m_doc.contentBounds(); if(b.isNull()) b=QRectF(QPointF(0,0),m_doc.pageSize);
    b.adjust(-40,-40,40,40); double zx=width()/b.width(), zy=height()/b.height(); m_zoom=qBound(0.05,qMin(zx,zy),8.0);
    m_origin=QPointF(width()/2.0,height()/2.0)-QPointF(b.center().x()*m_zoom,b.center().y()*m_zoom); emit zoomChanged(m_zoom); update(); }

// ---- hit testing -----------------------------------------------------------
int Canvas::hitShape(const QPointF& docPt) const {
    double tol = 5.0/m_zoom;
    for (int i=m_doc.shapes.size()-1;i>=0;--i){ const Shape& s=m_doc.shapes[i];
        if (s.locked || !m_doc.layerVisible(s.layer) || m_doc.layerLocked(s.layer)) continue;   // skip locked/hidden-layer objects
        if (s.hitTest(docPt,tol)) return i; }
    return -1;   // (unlock objects/layers via the Page & Layer or Name gallery)
}
// Select-behind (Alt+click): the hittable shape one step BELOW `above` in z-order at docPt, cycling back to
// the top when the bottom is reached — so repeated Alt+clicks walk down through a stack of overlapping objects.
int Canvas::hitShapeBehind(const QPointF& docPt, int above) const {
    double tol = 5.0/m_zoom; QVector<int> hits;   // collected top→bottom (highest index first)
    for (int i=m_doc.shapes.size()-1;i>=0;--i){ const Shape& s=m_doc.shapes[i];
        if (s.locked || !m_doc.layerVisible(s.layer) || m_doc.layerLocked(s.layer)) continue;
        if (s.hitTest(docPt,tol)) hits.push_back(i); }
    if (hits.isEmpty()) return -1;
    const int pos = hits.indexOf(above);
    return pos<0 ? hits.first() : hits[(pos+1)%hits.size()];   // next down; wrap to the topmost
}
// Select-into-group picking: topmost child of `group` (local→doc transform `gx`) under doc point d
// (fill hit, or within a stroke-width tolerance for thin/open shapes), or -1. Shared by press + double-click.
int Canvas::hitChildInGroup(const Shape& group, const QTransform& gx, const QPointF& d) const {
    const double tol = 6.0/m_zoom;
    for (int ci=group.children.size()-1; ci>=0; --ci){ const Shape& ch=group.children[ci];
        QPainterPath dp=(gx*ch.transform).map(ch.localPath());
        if (dp.contains(d)) return ci;
        QPainterPathStroker st; st.setWidth(qMax(2.0, ch.stroke.enabled?ch.stroke.width:2.0)+2*tol);
        if (st.createStroke(dp).contains(d)) return ci; }
    return -1;
}

// ===== Table cell interaction (T-2/T-3) =====================================
// Resolve a raw grid cell to its merge anchor (walk up/left over covered cells, span==0).
static void tableAnchorOf(const Table& t, int& r, int& c) {
    for (int cc=c; cc>=0; --cc) for (int rr=r; rr>=0; --rr) {
        const TableCell& x=t.at(rr,cc);
        if (x.rowSpan>0 && x.colSpan>0 && rr+qMax(1,x.rowSpan)>r && cc+qMax(1,x.colSpan)>c) { r=rr; c=cc; return; }
    }
}
bool Canvas::tableCellAt(int shapeIdx, const QPointF& docPt, int& r, int& c) const {
    if (shapeIdx<0 || shapeIdx>=m_doc.shapes.size()) return false;
    const Shape& s=m_doc.shapes[shapeIdx];
    if (s.type!=ShapeType::Table || !s.table.valid()) return false;
    const QPointF lp = s.transform.inverted().map(docPt);
    const Table& t=s.table;
    if (lp.x()<-1 || lp.y()<-1 || lp.x()>t.width()+1 || lp.y()>t.height()+1) return false;
    int cc=0; { double x=0; for(; cc<t.cols()-1; ++cc){ if(lp.x()<x+t.colW[cc]) break; x+=t.colW[cc]; } }
    int rr=0; { double y=0; for(; rr<t.rows()-1; ++rr){ if(lp.y()<y+t.rowH[rr]) break; y+=t.rowH[rr]; } }
    r=rr; c=cc; return true;
}
void Canvas::tableSelectCell(int shapeIdx, int r, int c, bool extend) {
    if (shapeIdx<0 || shapeIdx>=m_doc.shapes.size() || m_doc.shapes[shapeIdx].type!=ShapeType::Table) return;
    const Table& t=m_doc.shapes[shapeIdx].table; if(!t.valid()) return;
    r=qBound(0,r,t.rows()-1); c=qBound(0,c,t.cols()-1);
    m_tableShape=shapeIdx;
    if (extend && m_cellAR>=0) { m_cellBR=r; m_cellBC=c; }
    else { m_cellAR=m_cellBR=r; m_cellAC=m_cellBC=c; }
    m_sel={shapeIdx};
    emit selectionChanged(); update();
}
QRect Canvas::tableSelRange() const {
    if (m_tableShape<0 || m_cellAR<0) return QRect();
    int r0=qMin(m_cellAR,m_cellBR), r1=qMax(m_cellAR,m_cellBR);
    int c0=qMin(m_cellAC,m_cellBC), c1=qMax(m_cellAC,m_cellBC);
    return QRect(c0, r0, c1-c0+1, r1-r0+1);
}
void Canvas::tableSetCellText(int r, int c, const QString& txt) {
    if (m_tableShape<0) return; Table& t=m_doc.shapes[m_tableShape].table; if(!t.valid()) return;
    if (r<0||r>=t.rows()||c<0||c>=t.cols()) return;
    tableAnchorOf(t,r,c);
    pushUndo("Cell text"); t.at(r,c).text=txt; emit documentChanged(); update();
}
void Canvas::tableClearCells() {
    QRect sel=tableSelRange(); if(sel.isNull()||m_tableShape<0) return;
    Table& t=m_doc.shapes[m_tableShape].table; pushUndo("Clear cells");
    for(int r=sel.top();r<=sel.bottom();++r) for(int c=sel.left();c<=sel.right();++c) t.at(r,c).text.clear();
    emit documentChanged(); update();
}
void Canvas::tableSetCellBg(const QColor& col, bool clear) {
    QRect sel=tableSelRange(); if(sel.isNull()||m_tableShape<0) return;
    Table& t=m_doc.shapes[m_tableShape].table; pushUndo("Cell colour");
    for(int r=sel.top();r<=sel.bottom();++r) for(int c=sel.left();c<=sel.right();++c) t.at(r,c).bg = clear?QColor(0,0,0,0):col;
    emit documentChanged(); update();
}
void Canvas::tableSetCellAlign(int align) {
    QRect sel=tableSelRange(); if(sel.isNull()||m_tableShape<0) return;
    Table& t=m_doc.shapes[m_tableShape].table; pushUndo("Cell align");
    for(int r=sel.top();r<=sel.bottom();++r) for(int c=sel.left();c<=sel.right();++c) t.at(r,c).align=qBound(0,align,2);
    emit documentChanged(); update();
}
void Canvas::tableSetCellVAlign(int valign) {
    QRect sel=tableSelRange(); if(sel.isNull()||m_tableShape<0) return;
    Table& t=m_doc.shapes[m_tableShape].table; pushUndo("Cell vertical align");
    for(int r=sel.top();r<=sel.bottom();++r) for(int c=sel.left();c<=sel.right();++c) t.at(r,c).valign=qBound(0,valign,2);
    emit documentChanged(); update();
}
void Canvas::tableSetCellBold(bool bold) {
    QRect sel=tableSelRange(); if(sel.isNull()||m_tableShape<0) return;
    Table& t=m_doc.shapes[m_tableShape].table; pushUndo("Cell bold");
    for(int r=sel.top();r<=sel.bottom();++r) for(int c=sel.left();c<=sel.right();++c) t.at(r,c).bold=bold;
    emit documentChanged(); update();
}
void Canvas::tableSetHeaderRow(bool on) {
    if(m_tableShape<0) return;
    m_doc.shapes[m_tableShape].table.headerRow=on; pushUndo("Header row");
    emit documentChanged(); update();
}
bool Canvas::tableNavigate(int dr, int dc) {
    if (m_tableShape<0 || m_cellBR<0) return false;
    Table& t=m_doc.shapes[m_tableShape].table; if(!t.valid()) return false;
    int r=m_cellBR, c=m_cellBC;
    if (dc>0) {                              // Tab / right — linear next, wrap, append row past the end
        c+=dc; if(c>=t.cols()){ c=0; r++; }
        if(r>=t.rows()){ tableInsertRow(t.rows()); r=t.rows()-1; c=0; }
    } else if (dc<0) {                       // Shift+Tab / left
        c+=dc; if(c<0){ c=t.cols()-1; r--; } if(r<0){ r=0; c=0; }
    } else {                                 // up/down arrows
        r=qBound(0, r+dr, t.rows()-1);
    }
    tableSelectCell(m_tableShape, r, c, false);
    return true;
}
void Canvas::tableInsertRow(int at) {
    if (m_tableShape<0) return; Table& t=m_doc.shapes[m_tableShape].table; if(!t.valid()) return;
    const int R=t.rows(), C=t.cols(); at=qBound(0,at,R); pushUndo("Insert row");
    double h = t.rowH.isEmpty()?24.0:t.rowH[qMin(at,R-1)];
    QVector<TableCell> nc; nc.reserve((R+1)*C);
    for(int r=0;r<R;++r){ if(r==at) for(int c=0;c<C;++c) nc.append(TableCell()); for(int c=0;c<C;++c) nc.append(t.at(r,c)); }
    if(at>=R) for(int c=0;c<C;++c) nc.append(TableCell());
    t.cells=nc; t.rowH.insert(at, h);
    emit documentChanged(); update();
}
void Canvas::tableDeleteRow(int at) {
    if (m_tableShape<0) return; Table& t=m_doc.shapes[m_tableShape].table; if(!t.valid()||t.rows()<=1) return;
    at=qBound(0,at,t.rows()-1); pushUndo("Delete row");
    t.cells.erase(t.cells.begin()+at*t.cols(), t.cells.begin()+(at+1)*t.cols());
    t.rowH.removeAt(at);
    if(m_cellBR>=t.rows()) tableSelectCell(m_tableShape, t.rows()-1, m_cellBC, false);
    emit documentChanged(); update();
}
void Canvas::tableInsertCol(int at) {
    if (m_tableShape<0) return; Table& t=m_doc.shapes[m_tableShape].table; if(!t.valid()) return;
    at=qBound(0,at,t.cols()); pushUndo("Insert column");
    double w = t.colW.isEmpty()?60.0:t.colW[qMin(at,t.cols()-1)];
    QVector<TableCell> nc; nc.reserve(t.rows()*(t.cols()+1));
    for(int r=0;r<t.rows();++r){ for(int c=0;c<t.cols();++c){ if(c==at) nc.append(TableCell()); nc.append(t.at(r,c)); } if(at==t.cols()) nc.append(TableCell()); }
    t.colW.insert(at, w); t.cells=nc;
    emit documentChanged(); update();
}
void Canvas::tableDeleteCol(int at) {
    if (m_tableShape<0) return; Table& t=m_doc.shapes[m_tableShape].table; if(!t.valid()||t.cols()<=1) return;
    at=qBound(0,at,t.cols()-1); pushUndo("Delete column");
    QVector<TableCell> nc; nc.reserve(t.rows()*(t.cols()-1));
    for(int r=0;r<t.rows();++r) for(int c=0;c<t.cols();++c) if(c!=at) nc.append(t.at(r,c));
    t.colW.removeAt(at); t.cells=nc;
    if(m_cellBC>=t.cols()) tableSelectCell(m_tableShape, m_cellBR, t.cols()-1, false);
    emit documentChanged(); update();
}
void Canvas::tableSetAppearance(double borderW, const QColor& borderCol, double pad, double spacing) {
    if (m_tableShape<0) return; Table& t=m_doc.shapes[m_tableShape].table; if(!t.valid()) return;
    pushUndo("Table appearance");
    t.borderW=qMax(0.0,borderW); if(borderCol.isValid()) t.borderColor=borderCol;
    t.pad=qMax(0.0,pad); t.spacing=qMax(0.0,spacing);
    emit documentChanged(); update();
}
void Canvas::tableSetBg(const QColor& c, bool clear) {
    if (m_tableShape<0) return; Table& t=m_doc.shapes[m_tableShape].table; if(!t.valid()) return;
    pushUndo("Table background"); t.bg = clear?QColor(0,0,0,0):c;
    emit documentChanged(); update();
}
// Build a Table shape from parsed CSV rows and add it to the document; returns its shape index (or -1).
int Canvas::addTableFromRows(const QVector<QStringList>& rows) {
    if(rows.isEmpty()) return -1;
    // Defensive caps: a table this large is never a legitimate paste and a bigger one risks OOM.
    static const int MAX_ROWS=4096, MAX_COLS=256;
    const int nr=qMin(rows.size(), MAX_ROWS);
    int nc=0; for(int r=0;r<nr;++r) nc=qMax(nc, rows[r].size()); nc=qMin(nc, MAX_COLS); if(nc<1) return -1;
    const double cw=110.0, rh=26.0;
    Shape s; s.type=ShapeType::Table; s.name="Table";
    s.table=Table::grid(nr, nc, cw*nc, rh*nr);
    for(int r=0;r<nr;++r) for(int c=0;c<rows[r].size() && c<nc;++c) s.table.at(r,c).text=rows[r][c];
    s.fill.type=FillType::None; s.stroke.enabled=false;
    s.transform=QTransform::fromTranslate(m_doc.pageSize.width()/2 - cw*nc/2, 60);
    pushUndo("Import CSV table");
    m_doc.shapes.push_back(s); const int idx=m_doc.shapes.size()-1; m_sel={idx};
    emit documentChanged(); emit selectionChanged(); update();
    return idx;
}
// Parse a CSV file (comma-separated, "quoted" fields with "" escapes, CR/LF rows) into a Table.
bool Canvas::importCsvAsTable(const QString& path) {
    QFile f(path); if(!f.open(QIODevice::ReadOnly|QIODevice::Text)){ emit statusMessage("Could not open CSV file."); return false; }
    const QString text=QString::fromUtf8(f.readAll()); f.close();
    // Bound the parse itself (not just the built table): a pathological one-line file with millions of
    // commas would otherwise build a giant QStringList before addTableFromRows ever caps it.
    static const int PARSE_MAX_ROWS=4096, PARSE_MAX_COLS=256;
    QVector<QStringList> rows; QStringList cur; QString field; bool inQ=false;
    auto pushField=[&]{ if(cur.size()<PARSE_MAX_COLS) cur<<field; field.clear(); };
    for(int i=0;i<text.size() && rows.size()<PARSE_MAX_ROWS;++i){ const QChar ch=text[i];
        if(inQ){ if(ch=='"'){ if(i+1<text.size() && text[i+1]=='"'){ if(field.size()<1<<16) field+='"'; ++i; } else inQ=false; } else if(field.size()<1<<16) field+=ch; }
        else if(ch=='"') inQ=true;
        else if(ch==','){ pushField(); }
        else if(ch=='\n' || ch=='\r'){ if(ch=='\r' && i+1<text.size() && text[i+1]=='\n') ++i;
            pushField(); if(!(cur.size()==1 && cur[0].isEmpty())) rows<<cur; cur.clear(); }
        else if(field.size()<1<<16) field+=ch; }
    if((!field.isEmpty() || !cur.isEmpty()) && rows.size()<PARSE_MAX_ROWS){ pushField(); if(!(cur.size()==1 && cur[0].isEmpty())) rows<<cur; }
    if(rows.isEmpty()){ emit statusMessage("CSV file has no rows."); return false; }
    return addTableFromRows(rows)>=0;
}
// Even-out row heights (rows=true) or column widths (rows=false), preserving the overall table size.
void Canvas::tableDistribute(bool rows) {
    int ti = m_tableShape>=0 ? m_tableShape : (!m_sel.isEmpty() && m_doc.shapes[m_sel.first()].type==ShapeType::Table ? m_sel.first() : -1);
    if (ti<0) { emit statusMessage("Select a table to distribute its rows/columns evenly."); return; }
    Table& t=m_doc.shapes[ti].table; if(!t.valid()) return;
    auto& v = rows ? t.rowH : t.colW; if(v.size()<2) return;
    double total=0; for(double x:v) total+=x; const double each=total/v.size();
    pushUndo(rows?"Distribute rows":"Distribute columns");
    for(double& x:v) x=each;
    emit documentChanged(); emit selectionChanged(); update();
}
bool Canvas::tableMergeCells() {
    QRect sel=tableSelRange(); if(sel.isNull()||m_tableShape<0||sel.width()*sel.height()<2) return false;
    Table& t=m_doc.shapes[m_tableShape].table; pushUndo("Merge cells");
    QString joined; for(int r=sel.top();r<=sel.bottom();++r) for(int c=sel.left();c<=sel.right();++c){
        const QString& x=t.at(r,c).text; if(!x.isEmpty()){ if(!joined.isEmpty()) joined+=' '; joined+=x; } }
    for(int r=sel.top();r<=sel.bottom();++r) for(int c=sel.left();c<=sel.right();++c){
        TableCell& cell=t.at(r,c);
        if(r==sel.top()&&c==sel.left()){ cell.rowSpan=sel.height(); cell.colSpan=sel.width(); cell.text=joined; }
        else { cell.rowSpan=0; cell.colSpan=0; cell.text.clear(); } }
    emit documentChanged(); update(); return true;
}
void Canvas::tableSplitCells() {
    QRect sel=tableSelRange(); if(sel.isNull()||m_tableShape<0) return;
    Table& t=m_doc.shapes[m_tableShape].table; pushUndo("Split cells");
    // For each selected cell, resolve its merge anchor and un-cover the anchor's WHOLE spanned block.
    for(int r=sel.top();r<=sel.bottom()&&r<t.rows();++r) for(int c=sel.left();c<=sel.right()&&c<t.cols();++c){
        int ar=r,ac=c; tableAnchorOf(t,ar,ac);
        const int rs=qMax(1,t.at(ar,ac).rowSpan), cs=qMax(1,t.at(ar,ac).colSpan);
        for(int rr=ar; rr<ar+rs && rr<t.rows(); ++rr) for(int cc=ac; cc<ac+cs && cc<t.cols(); ++cc){ t.at(rr,cc).rowSpan=1; t.at(rr,cc).colSpan=1; }
    }
    emit documentChanged(); update();
}
QVector<QPointF> Canvas::handlePointsForRect(const QRectF& r) const {
    QVector<QPointF> pts;
    pts<<r.topLeft()<<QPointF(r.center().x(),r.top())<<r.topRight()<<QPointF(r.right(),r.center().y())
       <<r.bottomRight()<<QPointF(r.center().x(),r.bottom())<<r.bottomLeft()<<QPointF(r.left(),r.center().y());
    pts<<QPointF(r.center().x(), r.top()-26);   // rotate handle
    return pts;
}
QVector<QPointF> Canvas::handlePoints() const {
    QRectF b=selBoundsDoc(); if(b.isNull())return {};
    return handlePointsForRect(QRectF(toView(b.topLeft()), toView(b.bottomRight())));
}
bool Canvas::smartHandle(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (s.type!=ShapeType::Smart) return false;
    QRectF b=selBoundsDoc(); if(b.isNull()) return false;
    QRectF vr(toView(b.topLeft()), toView(b.bottomRight()));
    outView=QPointF(vr.right()+20, vr.top()-20);   // floats outside the top-right resize handle
    return true;
}
bool Canvas::smartHandle2(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (s.type!=ShapeType::Smart) return false;
    QRectF b=selBoundsDoc(); if(b.isNull()) return false;
    QRectF vr(toView(b.topLeft()), toView(b.bottomRight()));
    outView=QPointF(vr.right()+20, vr.bottom()+20);   // floats outside the bottom-right resize handle
    return true;
}
// The rounded-corner handle sits on the top edge of a single selected rectangle, inset from the top-left
// corner by the corner radius (kept a minimum distance out so it clears the corner resize handle). Dragging
// it along the edge sets cornerRadius. Returns false unless exactly one Rect is selected.
bool Canvas::cornerRadHandle(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (s.type!=ShapeType::Rect) return false;
    const double maxR=qMin(s.rect.width(), s.rect.height())/2.0;
    if (maxR<=0.5) return false;
    const double minDisp=18.0/qMax(0.05,m_zoom);   // keep the handle clear of the top-left corner resize handle
    const double r=qBound(0.0, s.cornerRadius, maxR);
    const double dx=qMin(maxR, qMax(r, minDisp));
    outView=toView(s.transform.map(s.rect.topLeft()+QPointF(dx,0)));
    return true;
}
// The shadow blur handle sits just past the blurred shadow's bottom-right corner (geometry bbox shifted by
// the shadow offset, pushed out by the blur). Dragging it out increases blur. docBounds() excludes effects
// so the base point is stable as blur changes.
bool Canvas::shadowBlurHandle(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (!s.shadow.on) return false;
    const QRectF b=s.docBounds(); if (b.isNull()) return false;
    const QPointF br=b.bottomRight()+s.shadow.offset;
    outView=toView(br+QPointF(s.shadow.blur, s.shadow.blur));
    return true;
}
// The FLOOR shadow's rake handle sits at the shadow's projected TIP — where the shape's top-centre lands
// after the floor transform (anchored at the baseline, sheared by floorShear, foreshortened by floorScale):
//   tip = ( centreX + offset.x - floorShear*H , baseY - floorScale*H ),  H = geometry height.
// Dragging it left/right leans the rake; up/down lengthens it. Only for shadowed FLOOR shapes (type 1).
bool Canvas::shadowRakeHandle(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (!s.shadow.on || s.shadow.type!=1) return false;
    const QRectF b=s.docBounds(); if(b.isNull() || b.height()<=0) return false;
    const double H=b.height();
    outView=toView(QPointF(b.center().x()+s.shadow.offset.x()-s.shadow.floorShear*H, b.bottom()-s.shadow.floorScale*H));
    return true;
}
// A small swatch of the current shadow colour, up-left of the shape's bounding box. Double-clicking it (or
// dropping a colour on it, or on the shape with the Shadow tool active) recolours the shadow. Not a drag
// handle — colour isn't a spatial value — so it opens the colour picker like the mesh-corner swatches.
bool Canvas::shadowColorSwatch(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (!s.shadow.on) return false;
    const QRectF b=s.docBounds(); if(b.isNull()) return false;
    outView=toView(b.topLeft())+QPointF(-16,-16);
    return true;
}
// A swatch of the current contour colour, up-left of the shape (the Contour tool has no other colour UI).
// Double-click it / drop a colour on it to recolour the contour. (color2 — the graduation outer colour — is
// set via the contour-graduation feature.) Feather has no colour: it softens the object's own edge.
bool Canvas::contourColorSwatch(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (!s.contour.on) return false;
    const QRectF b=s.docBounds(); if(b.isNull()) return false;
    outView=toView(b.topLeft())+QPointF(-16,-16);
    return true;
}
// The OUTER (graduation) contour colour swatch, stacked just below the inner one. Double-click to set
// contour.color2 (starts an inner→outer graduation); drawn hollow/dashed while color2 is unset.
bool Canvas::contourColor2Swatch(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (!s.contour.on) return false;
    const QRectF b=s.docBounds(); if(b.isNull()) return false;
    outView=toView(b.topLeft())+QPointF(-16,1);
    return true;
}
// The pie/arc start (endPt=false) or end (endPt=true) rim handle — only when the ellipse is a swept shape.
bool Canvas::ellipseRimHandle(bool endPt, QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (s.type!=ShapeType::Ellipse || s.ellipseMode==0) return false;
    const QRectF b=s.rect; const QPointF c=b.center();
    const double rx=b.width()/2.0, ry=b.height()/2.0;
    const double ang=(endPt? s.ellipseStart+s.ellipseSweep : s.ellipseStart)*M_PI/180.0;
    outView=toView(s.transform.map(QPointF(c.x()+rx*std::cos(ang), c.y()-ry*std::sin(ang))));   // Qt arc angle: CCW, y-up
    return true;
}
// Helper: the i-th polygon/star vertex in LOCAL coords (matches model.cpp localPath()).
// Node continuity: 0 cusp (independent handles), 1 smooth (collinear, lengths free),
// 2 symmetric (collinear + equal length). Reshapes the handle geometry to match the new type.
static void setNodeContinuity(PathNode& nd, int type) {
    if (type<=0) { nd.smooth=false; nd.symmetric=false; return; }          // cusp: leave handles as-is
    nd.smooth=true; nd.symmetric=(type>=2);
    if (!(nd.hasIn && nd.hasOut)) return;                                   // nothing to align (an end node)
    QPointF din=nd.anchor-nd.in, dout=nd.out-nd.anchor;
    double li=std::hypot(din.x(),din.y()), lo=std::hypot(dout.x(),dout.y());
    QPointF dir=din+dout; double dl=std::hypot(dir.x(),dir.y());            // averaged tangent direction
    if (dl<=1e-3) return;
    dir/=dl;
    if (nd.symmetric) { double m=(li+lo)/2.0; nd.in=nd.anchor-dir*m; nd.out=nd.anchor+dir*m; }
    else              { nd.in=nd.anchor-dir*li; nd.out=nd.anchor+dir*lo; }
}
static QPointF polyVertexLocal(const Shape& s, int i) {
    const QRectF b=s.rect; const QPointF c=b.center(); const double rx=b.width()/2.0, ry=b.height()/2.0;
    const int n=qMax(3,s.polySides); const double a0=-M_PI/2.0;
    if (s.polyStar){ const double a=a0+i*M_PI/n, r=(i&1)?s.polyStarDepth:1.0; return QPointF(c.x()+std::cos(a)*rx*r, c.y()+std::sin(a)*ry*r); }
    const double a=a0+i*2*M_PI/n; return QPointF(c.x()+std::cos(a)*rx, c.y()+std::sin(a)*ry);
}
// The edge-curvature handle: sits on the outward normal of the first edge's midpoint, pushed out by curvature.
bool Canvas::polyCurvHandle(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (s.type!=ShapeType::Polygon) return false;
    const QPointF Av=toView(s.transform.map(polyVertexLocal(s,0))), Bv=toView(s.transform.map(polyVertexLocal(s,1)));
    const QPointF Cv=toView(s.transform.map(s.rect.center())), midv=(Av+Bv)/2.0;
    QPointF ev=Bv-Av; double Lv=std::hypot(ev.x(),ev.y());
    QPointF nv = Lv>1e-9 ? QPointF(-ev.y()/Lv, ev.x()/Lv) : QPointF(0,-1);
    if (QPointF::dotProduct(nv, midv-Cv) < 0) nv=-nv;                 // point outward
    outView = midv + nv * (16.0 + s.polyCurvature*Lv*0.3642);
    return true;
}
// The star inner-vertex depth handle (star polygons only): its radius from centre == polyStarDepth.
bool Canvas::starDepthHandle(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (s.type!=ShapeType::Polygon || !s.polyStar) return false;
    outView=toView(s.transform.map(polyVertexLocal(s,1)));            // vertex 1 = first inner point
    return true;
}
// A swatch of the bevel's LIGHT colour, up-left of the shape (tints the bevel highlight; white = neutral).
// Double-click / drop a colour to change it. Placed clear of the depth/light-azimuth handles (bottom-right / orbit).
bool Canvas::bevelColorSwatch(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (!s.bevel.on) return false;
    const QRectF b=s.docBounds(); if(b.isNull()) return false;
    outView=toView(b.topLeft())+QPointF(-16,-16);
    return true;
}
// A swatch of the extrude's wall colour (the sides/back of the 3-D solid), up-left of the shape.
// Double-click / drop a colour to set it; unset (invalid) walls derive from the object's fill.
bool Canvas::extrudeColorSwatch(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (!s.extrude.on) return false;
    const QRectF b=s.docBounds(); if(b.isNull()) return false;
    outView=toView(b.topLeft())+QPointF(-16,-16);
    return true;
}
// One scalar handle for the active live-effect tool (Contour width / Bevel depth / Feather radius): it sits
// past the shape's bottom-right corner, pushed out by the current value. Dragging it out grows the value
// (Contour can go negative → inner). docBounds() excludes effects so the corner is stable as the value moves.
// Contour/Bevel need the effect switched on to show a handle; Feather is always available (0 = a hard edge).
bool Canvas::effectHandle(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    double val;
    if (m_tool==Tool::Contour){ if(!s.contour.on) return false; val=s.contour.width; }
    else if (m_tool==Tool::Bevel){ if(!s.bevel.on) return false; val=s.bevel.depth; }
    else if (m_tool==Tool::Feather){ val=s.feather; }
    else return false;
    const QRectF b=s.docBounds(); if(b.isNull()) return false;
    outView=toView(b.bottomRight()+QPointF(val,val));
    return true;
}
// The 3D-extrude vanishing-point handle: it sits at the shape centre + the extrusion vector
// depth·(cos angle, sin angle) — exactly the offset paintExtrude() projects the outline along. Dragging it
// sets BOTH depth (distance from the centre) and angle (direction). Shown only when the extrude is on.
bool Canvas::extrudeHandle(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (!s.extrude.on) return false;
    const QRectF b=s.docBounds(); if(b.isNull()) return false;
    const double a=s.extrude.angle*M_PI/180.0;
    outView=toView(b.center()+QPointF(s.extrude.depth*std::cos(a), s.extrude.depth*std::sin(a)));
    return true;
}
// The extrude light-azimuth handle orbits the shape centre at a fixed screen radius, in the direction the
// light comes from — (cos light, sin light), the same L vector paintExtrude() shades the side walls with.
// Dragging it around the centre sets extrude.light (0..360). Fixed screen radius so it's always reachable
// and never collides with the depth handle except in one exact configuration (depth handle wins there).
bool Canvas::extrudeLightHandle(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (!s.extrude.on) return false;
    const QRectF b=s.docBounds(); if(b.isNull()) return false;
    const double la=s.extrude.light*M_PI/180.0;
    outView=toView(b.center())+QPointF(58.0*std::cos(la), 58.0*std::sin(la));
    return true;
}
// The bevel light-azimuth handle, like the extrude one, but with the bevel's own convention: paintBevel()
// lights from (cos ang, -sin ang) (y-up azimuth), so the sun is placed with a NEGATED y and the drag reads
// atan2(-v.y, v.x). Orbits the shape centre at a fixed screen radius; sets bevel.light (0..360).
bool Canvas::bevelLightHandle(QPointF& outView) const {
    if (m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if (!s.bevel.on) return false;
    const QRectF b=s.docBounds(); if(b.isNull()) return false;
    const double la=s.bevel.light*M_PI/180.0;
    outView=toView(b.center())+QPointF(58.0*std::cos(la), -58.0*std::sin(la));
    return true;
}
QPointF Canvas::rotPivotDoc() const { return m_rotPivotSet ? m_rotPivotDoc : selBoundsDoc().center(); }
Canvas::Handle Canvas::hitHandle(const QPointF& w) const {
    if (m_sel.isEmpty()) return HNone;
    QPointF cr; if (cornerRadHandle(cr) && QLineF(w,cr).length()<9) return CornerRad;
    QPointF sh; if (smartHandle(sh) && QLineF(w,sh).length()<9) return SmartParam;
    QPointF sh2; if (smartHandle2(sh2) && QLineF(w,sh2).length()<9) return SmartParam2;
    QPointF tph; if (textPathStartHandle(tph) && QLineF(w,tph).length()<9) return TextPathStart;
    QPointF dch; if (dropCapHandle(dch) && QLineF(w,dch).length()<9) return DropCapSize;
    if (m_rotateMode){ if (QLineF(w, toView(rotPivotDoc())).length()<9) return RotPivot; }
    QVector<QPointF> pts=handlePoints();
    if (m_rotateMode){   // corners rotate, mid-edges skew (top/bottom = horizontal shear, left/right = vertical)
        const Handle rm[]={Rotate,SkewH,Rotate,SkewV,Rotate,SkewH,Rotate,SkewV};
        for (int i=0;i<8 && i<pts.size();++i) if (QLineF(w,pts[i]).length()<8) return rm[i];
        return HNone;
    }
    const Handle order[]={TL,T,TR,R,BR,B,BL,L,Rotate};
    for (int i=0;i<pts.size();++i) if (QLineF(w,pts[i]).length()<8) return order[i];
    return HNone;
}
// The 4 mesh-gradient corner positions in shape-local space (match render.cpp makeMeshImage):
// c0->p0, c1->p1, c2->p2, c3->p1+p2-p0.
QVector<QPointF> Canvas::meshCornerLocals(const Shape& s) const {
    const Fill& f=s.fill;
    return { f.p0, f.p1, f.p2, f.p1+f.p2-f.p0 };
}
int Canvas::hitMeshCorner(const QPointF& w) const {
    if (m_sel.size()!=1) return -1;
    const Shape& s=m_doc.shapes[m_sel.first()];
    if (s.fill.type!=FillType::Mesh) return -1;
    QVector<QPointF> lc=meshCornerLocals(s);
    for (int i=0;i<4;++i){ if (QLineF(w, toView(s.transform.map(lc[i]))).length()<9) return i; }
    return -1;
}
void Canvas::setMeshCornerColor(int corner, const QColor& c) {
    if (corner<0 || corner>3 || m_sel.size()!=1) return;
    Shape& s=m_doc.shapes[m_sel.first()]; if (s.fill.type!=FillType::Mesh) return;
    QVector<QColor>& mc=s.fill.meshColors;
    while (mc.size()<4) {   // pad a 3-colour mesh to 4 (compute the implied far corner) so corner 3 is editable
        if (mc.size()<3){ mc.append(mc.isEmpty()?QColor(Qt::black):mc.first()); continue; }
        QColor c0=mc[0],c1=mc[1],c2=mc[2];
        mc.append(QColor(qBound(0,c1.red()+c2.red()-c0.red(),255), qBound(0,c1.green()+c2.green()-c0.green(),255), qBound(0,c1.blue()+c2.blue()-c0.blue(),255)));
    }
    if (mc[corner]==c) return;
    pushUndo("Mesh corner colour"); mc[corner]=c; m_meshCorner=corner;
    emit documentChanged(); emit selectionChanged(); update();
}

void Canvas::setSmartParam(double p) {
    if (m_sel.size()!=1) return;
    Shape& s=m_doc.shapes[m_sel[0]]; if(s.type!=ShapeType::Smart) return;
    double np=qBound(2.0, p, 64.0);
    if (qAbs(np-s.smartParam)<1e-6) return;
    pushUndo("SmartShape parameter"); s.smartParam=np;
    emit documentChanged(); emit selectionChanged(); update();
}
void Canvas::setSmartParam2(double p) {
    if (m_sel.size()!=1) return;
    Shape& s=m_doc.shapes[m_sel[0]]; if(s.type!=ShapeType::Smart) return;
    double np=qBound(0.0, p, 1.0);
    if (s.smartParam2>=0.0 && qAbs(np-s.smartParam2)<1e-6) return;
    pushUndo("SmartShape parameter"); s.smartParam2=np;
    emit documentChanged(); emit selectionChanged(); update();
}

// ---- painting --------------------------------------------------------------
// Key capturing everything (besides document content) that changes the cached backdrop render.
Canvas::DocCacheKey Canvas::currentCacheKey() const {
    DocCacheKey k;
    k.xf = viewXf();
    k.pagesRect = m_doc.allPagesRect();
    k.gridStep = m_gridStep;
    k.pageColor = m_doc.pageColor.rgba();
    k.tacLimit = m_tacLimit; k.pageCount = m_doc.pageCount;
    k.outline = m_outlineView; k.softProof = m_softProof; k.gamut = m_gamutWarn; k.tac = m_tacWarn;
    k.grid = m_showGrid; k.gridIso = m_gridIso;
    k.enterTop = m_enterPath.isEmpty() ? -1 : m_enterPath.first();
    return k;
}
// True while a live drag is actively mutating document geometry — the cached document is stale each frame,
// so it must be rebuilt. Pure-overlay interactions (marquee, crop/straighten previews, pan) are excluded:
// pan is caught by the view-transform key; the others don't touch document content.
bool Canvas::manipulatingContent() const {
    if (m_rubber || m_cropMode || m_straightenMode) return false;
    if (!m_pressMoved) return false;    // a drag armed on press but not yet moved → geometry unchanged, cache still valid
    return m_dragging || m_rotating || m_penHandleDrag || m_bendActive
        || m_mouldDrag >= 0 || m_nodeSub >= 0 || !m_freePts.isEmpty();
}

// ---- Drag proxy (dynamic-object layer) ------------------------------------------------------------
// The proxy applies ONLY to a whole-selection affine gesture: move (no handle), corner/side resize, rotate
// or skew of a TOP-LEVEL selection. Those all rewrite each target's transform as startXf·M for one shared M,
// so the moving pixels are a rigid picture we can capture once and re-blit under M every frame. Everything
// else (node edits, SmartShape params, gradient/handle drags, in-group child edits, text-frame reflow, or a
// document with resolved text fields/chains) keeps the existing per-frame full rebuild.
bool Canvas::dragProxyEligible() const {
    if (!m_dragProxyEnabled) return false;
    if (m_tool != Tool::Select) return false;
    if (inGroup()) return false;                 // in-group child render lives inside the group's paint; can't isolate by top-level address
    if (m_sel.isEmpty()) return false;
    if (hasTextFields() || hasTextChains()) return false;   // paintBackdrop renders a resolved COPY → addresses wouldn't match the skip/only set
    switch (m_activeHandle) {                    // whole-selection affine handles only
        case HNone: case TL: case T: case TR: case R: case BR: case B: case BL: case L:
        case Rotate: case SkewH: case SkewV: break;
        default: return false;                   // SmartParam/CornerRad/TextPathStart/gradient handles/etc.
    }
    // A side handle on a single text FRAME reflows the text (mutates rect, not transform) → M would be
    // identity and the proxy would look frozen. Exclude it (corner handles still scale, so those stay eligible).
    if (m_sel.size()==1 && m_sel[0]>=0 && m_sel[0]<m_doc.shapes.size()) {
        const Shape& s = m_doc.shapes[m_sel[0]];
        if (s.type==ShapeType::Text && s.textFrame &&
            (m_activeHandle==T||m_activeHandle==R||m_activeHandle==B||m_activeHandle==L)) return false;
    }
    return true;
}

// Capture the static (rest-of-document) and moving (selection) bitmaps at the CURRENT geometry, which
// becomes the proxy's baseline. Returns false (→ fall back to full rebuild) if ineligible, the reference
// transform is singular, or the moving bitmap would be larger than the memory cap.
bool Canvas::buildDragProxies() {
    if (!dragProxyEligible()) return false;
    QVector<Shape*> tg = selTargets();
    if (tg.isEmpty()) return false;
    bool inv=false; QTransform baseInv = tg[0]->transform.inverted(&inv);
    if (!inv) return false;                      // zero-scale reference → can't define M
    (void)baseInv;

    QSet<const void*> movers;
    for (Shape* s : tg) movers.insert(s);

    // Moving selection doc-bounds + a conservative effect margin, so shadows/feather at the edge are captured.
    QRectF db;
    double margin = 4.0;
    for (Shape* s : tg) { db = db.isNull() ? s->docBounds() : db.united(s->docBounds()); margin = qMax(margin, shapeRenderReach(*s)); }
    if (db.isNull()) return false;
    db = db.adjusted(-margin, -margin, margin, margin);

    const qreal dpr = devicePixelRatioF();
    const QRectF vb = viewXf().mapRect(db);      // view is translate+uniform-scale only ⇒ axis-aligned, tight
    QSize dev(int(vb.width()*dpr + 0.5), int(vb.height()*dpr + 0.5));
    if (dev.isEmpty()) return false;
    // Cap the CAPTURE resolution so the one-time build stays fast regardless of content complexity or display
    // DPI (a heavy group on a retina screen would otherwise take seconds to render once, freezing the UI). The
    // bitmap is drawn back at the same LOGICAL size, so the only visible effect is a slightly softer live
    // preview of a very large selection; the crisp vector render returns the instant the drag ends.
    qreal pdpr = dpr;
    const qint64 area = (qint64)dev.width()*dev.height();
    if (area > kDragProxyBudgetPx) {
        const qreal q = std::sqrt(double(kDragProxyBudgetPx)/double(area));
        dev = QSize(qMax(1, int(dev.width()*q)), qMax(1, int(dev.height()*q)));
        pdpr = dpr * q;                          // logical size = dev/pdpr stays == vb.size()
    }

    // Moving proxy: object-bounds, transparent, only the moving shapes at build-time geometry.
    m_dragMovingProxy = QImage(dev, QImage::Format_ARGB32_Premultiplied);
    m_dragMovingProxy.setDevicePixelRatio(pdpr);
    m_dragMovingProxy.fill(Qt::transparent);
    {
        QPainter mp(&m_dragMovingProxy);
        mp.translate(-vb.topLeft());             // logical view coords → image coords (shift vb.topLeft to 0,0)
        mp.setTransform(viewXf(), true);
        mp.setRenderHint(QPainter::Antialiasing, true);
        mp.setRenderHint(QPainter::SmoothPixmapTransform, true);
        setOutlineRenderMode(m_outlineView);   // match the static proxy's view mode (wireframe drags stay wireframe)
        setScreenRenderMode(true);
        setRenderOnlySet(movers);
        paintDocument(mp, m_doc);
        setRenderOnlySet(QSet<const void*>());
        setScreenRenderMode(false);
        setOutlineRenderMode(false);
    }
    m_dragProxyOrigin = vb.topLeft();
    m_dragProxyBaseXf = tg[0]->transform;

    // Cache the selection silhouette (doc space, at this baseline geometry) so drawSelection can remap it by M
    // each frame instead of rebuilding each selected shape's path — for a group that path traverses every child.
    m_dragSelOutline = QPainterPath();
    for (Shape* s : tg) m_dragSelOutline.addPath(Document::docPath(*s));

    // Static proxy: screen-sized full backdrop with the moving shapes skipped.
    const QSize sdev(int(width()*dpr + 0.5), int(height()*dpr + 0.5));
    if (sdev.isEmpty()) return false;
    m_dragStaticProxy = QImage(sdev, QImage::Format_ARGB32_Premultiplied);
    m_dragStaticProxy.setDevicePixelRatio(dpr);
    m_dragStaticProxy.fill(Qt::transparent);
    {
        m_dragProxySkip = movers;                // paintBackdrop's document layer honours this
        QPainter sp(&m_dragStaticProxy);
        paintBackdrop(sp);
        sp.end();
        m_dragProxySkip.clear();
    }

    m_dragProxyViewXf   = viewXf();
    m_dragProxyViewSize = size();
    m_dragProxyActive   = true;
    return true;
}

double Canvas::snapAdjustProbeMs(int iters) {   // time the per-frame snap scan that the move drag runs
    if (iters < 1) iters = 1;
    QRectF bb = selBoundsDoc(); if (bb.isNull()) bb = QRectF(QPointF(0,0), m_doc.pageSize);
    QVector<double> gx, gy; QElapsedTimer t; t.start();
    for (int i=0;i<iters;++i){ gx.clear(); gy.clear(); volatile double s = snapAdjust(bb.translated(i%7,i%5), gx, gy).x(); (void)s; }
    return t.nsecsElapsed()/1e6/iters;
}

void Canvas::clearDragProxy() {
    if (!m_dragProxyActive && !m_dragProxyTried && m_dragStaticProxy.isNull() && m_dragMovingProxy.isNull()) return;
    m_dragProxyActive = false;
    m_dragProxyTried  = false;
    m_dragStaticProxy = QImage();
    m_dragMovingProxy = QImage();
    m_dragSelOutline  = QPainterPath();
    m_dragProxySkip.clear();
    m_cacheDirty = true;                         // next paint does a full, correct vector re-render
}

// Blit the static backdrop, then the moving-selection bitmap transformed by S = viewInv·M·view (mapping its
// build-time screen position to the current one), then the live overlays. No document rasterisation.
void Canvas::paintDragProxy(QPainter& g) {
    // The static proxy is the full backdrop (opaque graphite gradient covers the whole widget), so blit it as a
    // straight copy rather than an alpha blend, then switch back to SourceOver for the moving bitmap + overlays.
    g.setCompositionMode(QPainter::CompositionMode_Source);
    g.drawImage(0, 0, m_dragStaticProxy);
    g.setCompositionMode(QPainter::CompositionMode_SourceOver);
    QVector<Shape*> tg = selTargets();
    if (!tg.isEmpty()) {
        const QTransform M = m_dragProxyBaseXf.inverted() * tg[0]->transform;   // shared doc-space delta since build
        const QTransform S = viewXf().inverted() * M * viewXf();                // drag-start view coords → current view coords
        g.save();
        g.setRenderHint(QPainter::SmoothPixmapTransform, true);
        g.setTransform(S, true);
        g.drawImage(m_dragProxyOrigin, m_dragMovingProxy);
        g.restore();
    }
    paintOverlays(g);
}

// Headless perf probe: build the proxy for the current selection, then time a proxy-composite frame vs a
// full document rebuild (what a live drag used to cost every frame). Restores all state before returning.
bool Canvas::dragProxyProbe(int iters, double& proxyMs, double& fullMs) {
    proxyMs = fullMs = 0.0;
    if (iters < 1) iters = 1;
    if (m_sel.isEmpty()) return false;
    const qreal dpr = devicePixelRatioF();
    const QSize dev = size() * dpr;
    if (dev.isEmpty()) return false;
    QImage scratch(dev, QImage::Format_ARGB32_Premultiplied); scratch.setDevicePixelRatio(dpr);

    const auto savedTool = m_tool; const auto savedH = m_activeHandle;
    const bool savedDrag = m_dragging, savedRot = m_rotating;
    m_tool = Tool::Select; m_activeHandle = HNone; m_dragging = true; m_rotating = false;   // simulate a move drag

    QVector<Shape*> tg = selTargets();
    QVector<QTransform> saved; for (Shape* s : tg) saved.push_back(s->transform);

    const bool ok = buildDragProxies();          // baseline captured at the current geometry
    if (ok) {
        for (Shape* s : tg) s->transform = s->transform * QTransform::fromTranslate(40, 30);   // now M != identity
        QElapsedTimer t; t.start();
        for (int i = 0; i < iters; ++i) { QPainter g(&scratch); paintDragProxy(g); }
        proxyMs = t.nsecsElapsed() / 1e6 / iters;
        t.restart();                             // what the old per-frame path did: full reflow + backdrop + overlays
        for (int i = 0; i < iters; ++i) { scratch.fill(Qt::transparent); QPainter g(&scratch); reflowText(); paintBackdrop(g); paintOverlays(g); }
        fullMs = t.nsecsElapsed() / 1e6 / iters;
    }

    for (int k = 0; k < tg.size() && k < saved.size(); ++k) tg[k]->transform = saved[k];
    clearDragProxy();
    m_tool = savedTool; m_activeHandle = savedH; m_dragging = savedDrag; m_rotating = savedRot;
    m_cacheDirty = true;
    return ok;
}

// Isolate the selection-outline cost: time drawSelection using the cached outline vs the per-frame rebuild.
bool Canvas::dragOutlineProbe(int iters, double& cachedMs, double& rebuiltMs) {
    cachedMs = rebuiltMs = 0.0;
    if (iters < 1) iters = 1;
    if (m_sel.isEmpty()) return false;
    const qreal dpr = devicePixelRatioF();
    const QSize dev = size() * dpr;
    if (dev.isEmpty()) return false;
    QImage scratch(dev, QImage::Format_ARGB32_Premultiplied); scratch.setDevicePixelRatio(dpr);

    const auto savedTool = m_tool; const auto savedH = m_activeHandle;
    const bool savedDrag = m_dragging, savedRot = m_rotating;
    m_tool = Tool::Select; m_activeHandle = HNone; m_dragging = true; m_rotating = false;

    QVector<Shape*> tg = selTargets();
    QVector<QTransform> saved; for (Shape* s : tg) saved.push_back(s->transform);

    const bool ok = buildDragProxies();
    if (ok) {
        for (Shape* s : tg) s->transform = s->transform * QTransform::fromTranslate(30, 20);
        QElapsedTimer t;
        m_dragProxyActive = true;  t.start();                       // cached: remap the drag-start outline
        for (int i = 0; i < iters; ++i) { QPainter g(&scratch); drawSelection(g); }
        cachedMs = t.nsecsElapsed() / 1e6 / iters;
        m_dragProxyActive = false; t.restart();                     // rebuilt: docPath()/selBoundsDoc() every frame
        for (int i = 0; i < iters; ++i) { m_selCacheValid = false;   // force the UNCACHED cost (the static-outline cache would otherwise serve it)
            QPainter g(&scratch); drawSelection(g); }
        rebuiltMs = t.nsecsElapsed() / 1e6 / iters;
        m_selCacheValid = false;                                     // leave the cache cold (the moved transforms above are transient)
        m_dragProxyActive = true;                                   // restore so clearDragProxy tears down cleanly
    }

    for (int k = 0; k < tg.size() && k < saved.size(); ++k) tg[k]->transform = saved[k];
    clearDragProxy();
    m_tool = savedTool; m_activeHandle = savedH; m_dragging = savedDrag; m_rotating = savedRot;
    m_cacheDirty = true;
    return ok;
}

Canvas::RenderProbe Canvas::renderProbe(int iters) {
    RenderProbe r;
    if (iters < 1) iters = 1;
    const qreal dpr = devicePixelRatioF();
    const QSize dev = size() * dpr;
    if (dev.isEmpty()) return r;
    QImage scratch(dev, QImage::Format_ARGB32_Premultiplied); scratch.setDevicePixelRatio(dpr);
    // Build the cache once so the cached path measures a realistic blit.
    m_screenCache = QImage(dev, QImage::Format_ARGB32_Premultiplied); m_screenCache.setDevicePixelRatio(dpr);
    m_screenCache.fill(Qt::transparent);
    { QPainter g(&m_screenCache); paintBackdrop(g); }
    m_cacheKey = currentCacheKey(); m_cacheDirty = false;
    QElapsedTimer t;
    t.start();                                              // uncached: reflow + full backdrop rasterise
    for (int i = 0; i < iters; ++i) { scratch.fill(Qt::transparent); QPainter g(&scratch); reflowText(); paintBackdrop(g); }
    r.fullMs = t.nsecsElapsed() / 1e6 / iters;
    t.restart();                                            // cached: blit + cheap overlays
    for (int i = 0; i < iters; ++i) { QPainter g(&scratch); g.drawImage(0, 0, m_screenCache); paintOverlays(g); }
    r.cachedMs = t.nsecsElapsed() / 1e6 / iters;
    return r;
}

void Canvas::paintEvent(QPaintEvent*) {
    const qreal dpr = devicePixelRatioF();
    const QSize dev(int(width()*dpr + 0.5), int(height()*dpr + 0.5));

    // --- Drag proxy fast path: while a whole-selection affine drag is live, composite the two cached
    // bitmaps instead of re-rasterising the whole document each frame.
    if (!manipulatingContent()) {
        if (m_dragProxyActive || m_dragProxyTried) clearDragProxy();   // drag ended (any path) → drop proxy state
    } else if (m_dragProxyActive) {
        // Bail out (and full-rebuild this frame) if the view panned/zoomed or the widget resized since the
        // proxy was captured — the static bitmap and origin would no longer line up.
        if (viewXf() != m_dragProxyViewXf || size() != m_dragProxyViewSize) clearDragProxy();
        else { QPainter g(this); paintDragProxy(g); return; }
    } else if (!m_dragProxyTried && dragProxyEligible()) {
        // First frame of an eligible drag: try to build the proxy. On success, paint through it immediately.
        m_dragProxyTried = true;
        reflowText();
        if (buildDragProxies()) { QPainter g(this); paintDragProxy(g); return; }
        // else: ineligible/too big → fall through to the normal (full-rebuild) path below for the rest of the drag
    }

    // Interactive-zoom bitmap preview: while a wheel-zoom gesture is live, blit the last rendered backdrop scaled
    // from the view it was rendered at (m_previewCacheXf) to the current view, instead of re-rendering the whole
    // document (which can take ~1s at high zoom and freeze the UI). Near-instant; the settle timer does ONE crisp
    // re-render when motion stops. Guarded so it never runs without a usable, same-size cache.
    if (m_previewZoom && !m_screenCache.isNull() && m_screenCache.size()==dev
        && m_cacheKey.xf != viewXf() && m_cacheKey.xf.isInvertible()) {   // only if there's an actual zoom delta to preview
        QPainter g(this);
        g.fillRect(rect(), QColor(0x2A,0x2D,0x34));                       // pasteboard fill for areas the scaled bitmap doesn't cover
        g.setRenderHint(QPainter::SmoothPixmapTransform, true);
        g.setTransform(m_cacheKey.xf.inverted() * viewXf());             // cached (rendered-view) pixels → current view
        g.drawImage(0, 0, m_screenCache);
        g.resetTransform();
        paintOverlays(g);
        return;
    }

    const DocCacheKey key = currentCacheKey();
    // Reuse the cached backdrop+document unless the content, view, widget size or a view-mode toggle
    // changed, or a live geometry drag is mutating the document this frame.
    if (m_cacheDirty || m_screenCache.size() != dev || !(m_cacheKey == key) || manipulatingContent()) {
        reflowText();                   // keep text repel/anchor layout current (it feeds the render)
        if (m_screenCache.size() != dev) m_screenCache = QImage(dev, QImage::Format_ARGB32_Premultiplied);
        m_screenCache.setDevicePixelRatio(dpr);
        m_screenCache.fill(Qt::transparent);
        QElapsedTimer bt; bt.start();
        QPainter cg(&m_screenCache);
        paintBackdrop(cg);
        cg.end();
        m_lastBackdropMs = bt.nsecsElapsed()/1e6;   // wall-clock of this full render → adaptive preview trigger
        m_cacheKey = key; m_cacheDirty = false;
    }
    QPainter g(this);
    g.drawImage(0, 0, m_screenCache);
    paintOverlays(g);
}

// Background + page(s) + grid + the document content layer: the expensive part, rendered into the cache.
void Canvas::paintBackdrop(QPainter& g) {
    // XarQ grad-canvas: a soft radial from the top-centre so the pasteboard reads as machined graphite.
    { const QRectF r=rect(); QRadialGradient cbg(QPointF(r.center().x(), r.top()), r.height()*1.25);
      cbg.setColorAt(0.0, QColor(0x32,0x36,0x3E)); cbg.setColorAt(1.0, QColor(0x22,0x25,0x2B));
      g.fillRect(r, cbg); }

    // page(s) — stacked vertically on the pasteboard. The artboard rect spans all pages.
    g.setRenderHint(QPainter::Antialiasing);
    QRectF page;   // first page (used for the content clip union below)
    for (int pi=0; pi<m_doc.pageCount; ++pi) {
        QRectF pr=m_doc.pageRect(pi);
        QRectF pv(toView(pr.topLeft()), toView(pr.bottomRight()));
        if(pi==0) page=pv;
        g.setPen(Qt::NoPen);
        // soft drop shadow — layered low-alpha rects (approximates a blur), biased downward (light from top)
        for(int s=12;s>=1;--s){ g.setBrush(QColor(0,0,0,5)); g.drawRoundedRect(pv.adjusted(-s, -s+4, s, s+6), s, s); }
        g.setBrush(m_doc.pageColor); g.drawRect(pv);
        if(m_doc.pageCount>1){ g.setPen(QColor(150,155,165)); QFont f=g.font(); f.setPixelSize(10); g.setFont(f);
            g.drawText(QPointF(pv.left(), pv.top()-4), QString("Page %1").arg(pi+1)); }
    }
    drawGrid(g);   // grid drawn on TOP of the page(s) (when enabled), under the document content
    // union of all page rects for the content clip (so objects spanning pages still show)
    QRectF allv(toView(m_doc.allPagesRect().topLeft()), toView(m_doc.allPagesRect().bottomRight()));
    page = allv;

    // document, clipped to the page. Render to a TRANSPARENT offscreen layer first, then composite it over
    // the (white) page — so backdrop-sensitive blend/transparency modes see the document's own transparent
    // backdrop, exactly as the PNG export does. (Painting straight onto the opaque page made a blend on
    // Watch4's gold case rim read the white page as its backdrop and come out magenta; the export, on a
    // transparent canvas, renders it correctly gold.)
    {
        const qreal dpr = devicePixelRatioF();
        QImage layer(size()*dpr, QImage::Format_ARGB32_Premultiplied);
        layer.setDevicePixelRatio(dpr);
        layer.fill(Qt::transparent);
        QPainter lp(&layer);
        lp.setClipRect(page.adjusted(-1,-1,1,1));
        lp.setTransform(viewXf(), true);
        lp.setRenderHint(QPainter::Antialiasing, true);
        lp.setRenderHint(QPainter::SmoothPixmapTransform, true);
        setOutlineRenderMode(m_outlineView);   // screen-only wireframe (reset right after so exports are unaffected)
        setScreenRenderMode(true);             // non-printing layers still show on the canvas (only print/export hide them)
        // Viewport culling (canvas only): skip shapes wholly outside the visible doc rect. Worthwhile only when
        // there are many shapes; cleared immediately after so export/thumbnail paths never cull.
        const QRectF visibleDoc = viewXf().inverted().mapRect(QRectF(0,0,width(),height()));
        if(m_cullEnabled && m_doc.shapes.size()>32)
            setRenderCullRect(visibleDoc);   // culling compares against shape DOC bounds → doc-space rect
        // Always clip live effect buffers (blur/feather/bevel/…) to the visible area on the canvas, so their
        // per-frame cost is bounded by the screen — not the shape's device extent, which explodes at high zoom
        // and froze the UI. This rect is in the painter's DEVICE space (the widget in logical px) — the same
        // space the effect functions compute their device buffers in, for top-level AND grouped shapes — so it
        // must NOT be transformed by any per-shape/group matrix. Export/thumbnail never sets it ⇒ byte-identical.
        setRenderViewport(QRectF(0, 0, width(), height()));
        setRenderSkipSet(m_dragProxySkip);     // non-empty only while building the static drag proxy (skip the movers)
        if(hasTextFields()||hasTextChains()){ Document rd=resolvedForRender(); paintDocument(lp, rd); } else paintDocument(lp, m_doc);
        setRenderSkipSet(QSet<const void*>());
        setRenderCullRect(QRectF());
        setRenderViewport(QRectF());
        // Group-isolation ("enter group") mode: dim the whole scene, then re-draw the entered group bright on top,
        // so it's clear the user is editing INSIDE it. Clicking the faded area or pressing Esc leaves (input handler).
        if (inGroup() && m_enterPath.first()>=0 && m_enterPath.first()<m_doc.shapes.size()) {
            lp.fillRect(visibleDoc, QColor(16,18,23,140));                    // translucent scrim over everything (doc space)
            setRenderViewport(QRectF(0,0,width(),height()));
            setRenderOnlySet({ &m_doc.shapes[m_enterPath.first()] });         // draw ONLY the entered group, at full opacity
            paintDocument(lp, m_doc);
            setRenderOnlySet(QSet<const void*>());
            setRenderViewport(QRectF());
        }
        setScreenRenderMode(false);
        setOutlineRenderMode(false);
        lp.end();
        if (m_softProof) {                            // view-only CMYK gamut preview (export path untouched)
            QImage proofed = Icc::instance().available() ? Icc::instance().softProof(layer.convertToFormat(QImage::Format_ARGB32)) : QImage();
            if (!proofed.isNull()) layer = proofed.convertToFormat(QImage::Format_ARGB32_Premultiplied);  // real ICC proof
            else softProofImage(layer);               // naive fallback (no Little-CMS)
        }
        if (m_gamutWarn) gamutWarnImage(layer);       // flag out-of-CMYK-gamut pixels grey (view-only)
        if (m_tacWarn)   tacWarnImage(layer, m_tacLimit);   // flag pixels over the total-ink-coverage limit (view-only)
        g.drawImage(0, 0, layer);
    }

    g.setPen(QPen(QColor(0,0,0,40),1)); g.setBrush(Qt::NoBrush); g.drawRect(page);
}

// Guides, selection, tool handles, crop/straighten overlays and rulers — cheap, redrawn every frame on
// top of the cached backdrop so interaction (hover, selection, handle drags) never re-rasterises the document.
void Canvas::paintOverlays(QPainter& g) {
    // persistent user guides (cyan), drawn over content
    if(!m_guideHpos.isEmpty() || !m_guideVpos.isEmpty()){
        g.setPen(QPen(QColor(0,190,210,170),1,Qt::DashLine));
        for(double gy: m_guideHpos){ double vy=toView(QPointF(0,gy)).y(); g.drawLine(QPointF(0,vy),QPointF(width(),vy)); }
        for(double gx: m_guideVpos){ double vx=toView(QPointF(gx,0)).x(); g.drawLine(QPointF(vx,0),QPointF(vx,height())); }
    }

    drawSelection(g);
    drawToolOverlay(g);

    // smart alignment guides (magenta) that fired during the current drag
    if (!m_guideX.isEmpty() || !m_guideY.isEmpty()) {
        g.setRenderHint(QPainter::Antialiasing,false);
        g.setPen(QPen(QColor(255,45,210), 1));
        for (double x : m_guideX){ double vx=toView(QPointF(x,0)).x(); g.drawLine(QPointF(vx,0),QPointF(vx,height())); }
        for (double y : m_guideY){ double vy=toView(QPointF(0,y)).y(); g.drawLine(QPointF(0,vy),QPointF(width(),vy)); }
    }

    // Interactive crop overlay: dim outside the crop rect, dashed border + 8 handles.
    if (m_cropMode) {
        const QRectF vr(toView(m_cropRectDoc.topLeft()), toView(m_cropRectDoc.bottomRight()));
        g.setRenderHint(QPainter::Antialiasing,false);
        QPainterPath outside; outside.addRect(rect()); QPainterPath in; in.addRect(vr.normalized());
        g.setBrush(QColor(0,0,0,110)); g.setPen(Qt::NoPen); g.drawPath(outside.subtracted(in));
        g.setBrush(Qt::NoBrush); g.setPen(QPen(Qt::white,1,Qt::DashLine)); g.drawRect(vr.normalized());
        g.setPen(QPen(QColor(30,30,30),1)); g.setBrush(Qt::white);
        for (const QPointF& pd : cropHandlesDoc(m_cropRectDoc)){ const QPointF pv=toView(pd); g.drawRect(QRectF(pv.x()-4,pv.y()-4,8,8)); }
    }

    // Interactive straighten overlay: the reference line (should-be-level) + its two grip handles + angle readout.
    if (m_straightenMode) {
        g.setRenderHint(QPainter::Antialiasing,true);
        const QPointF va=toView(m_straightenA), vb=toView(m_straightenB);
        g.setPen(QPen(QColor(0,0,0,150),3)); g.drawLine(va,vb);                 // dark casing for contrast
        g.setPen(QPen(QColor(45,207,120),1.6)); g.drawLine(va,vb);              // green reference line
        g.setBrush(Qt::white); g.setPen(QPen(QColor(45,207,120),1.6));
        g.drawEllipse(va,5,5); g.drawEllipse(vb,5,5);
        const double deg=straightenAngleDeg();
        g.setPen(QPen(Qt::white,1)); QFont f=g.font(); f.setPointSizeF(9); g.setFont(f);
        g.drawText(QRectF((va+vb)/2.0+QPointF(-40,-24),QSizeF(80,16)), Qt::AlignCenter,
                   QString("%1°").arg(-deg,0,'f',1));
    }

    drawRulers(g);
    drawRadialMenu(g);   // the Ctrl pie menu floats above everything, including the rulers
}

static const int kRuler = 18;   // ruler strip thickness in px

// doc-pixels per unit (doc space = 1/96 inch per px)
double Canvas::unitFactor(Units u){
    switch(u){ case Units::Pixels: return 1.0; case Units::Inches: return 96.0;
        case Units::Millimetres: return 96.0/25.4; case Units::Centimetres: return 960.0/25.4;
        case Units::Points: return 96.0/72.0; } return 1.0; }
QString Canvas::unitSuffix(Units u){
    switch(u){ case Units::Pixels: return "px"; case Units::Inches: return "in";
        case Units::Millimetres: return "mm"; case Units::Centimetres: return "cm"; case Units::Points: return "pt"; } return "px"; }
QString Canvas::fmtLen(double docPx, bool suffix) const {
    double v=docPx/unitFactor(m_units);
    int dec = (m_units==Units::Pixels)?0 : (m_units==Units::Points?1:2);
    QString s=QString::number(v,'f',dec);
    return suffix ? s+" "+unitSuffix(m_units) : s; }

void Canvas::setPageColor(const QColor& c){
    if(!c.isValid() || c==m_doc.pageColor) return;
    pushUndo("Page colour"); m_doc.pageColor=c; emit documentChanged(); update();
}
void Canvas::drawGrid(QPainter& g) {
    if (!m_showGrid) return;
    double pxStep = m_gridStep*m_zoom; if (pxStep < 6) return;       // sub-divide too-fine grids away
    if (m_gridIso) {                                                  // isometric grid: verticals + ±30° diagonals
        const double m=std::tan(30.0*M_PI/180.0), L=width()+height()+200;
        const QPointF O=toView(QPointF(0,0));                        // lock the mesh to the document origin
        double phase=std::fmod(O.x(),pxStep); if(phase<0) phase+=pxStep;
        g.setPen(QPen(QColor(122,134,150,60),1));
        for(double c=phase-2.0*height(); c<width()+height(); c+=pxStep){
            g.drawLine(QPointF(c-L, O.y()-m*L), QPointF(c+L, O.y()+m*L));   // 30° (down-right)
            g.drawLine(QPointF(c-L, O.y()+m*L), QPointF(c+L, O.y()-m*L));   // 150° (up-right)
            if(c>=0 && c<=width()) g.drawLine(QPointF(c,0),QPointF(c,height()));   // verticals
        }
        return;
    }
    int sub = 1; while (pxStep*sub < 60) sub++;                       // every `sub`-th line is a major line
    double x0d = std::ceil(toDoc(QPointF(kRuler,0)).x()/m_gridStep)*m_gridStep;
    double y0d = std::ceil(toDoc(QPointF(0,kRuler)).y()/m_gridStep)*m_gridStep;
    // neutral blue-grey so the grid reads on BOTH the (white) page and the dark pasteboard now that it's
    // drawn on top of the page
    auto pen=[&](bool major){ g.setPen(QPen(QColor(122,134,150, major?86:48),1)); };
    for (double dx=x0d; ; dx+=m_gridStep){ double vx=toView(QPointF(dx,0)).x(); if(vx>width()) break;
        int k=qRound(dx/m_gridStep); pen(sub<=1 || k%sub==0); g.drawLine(QPointF(vx,0),QPointF(vx,height())); }
    for (double dy=y0d; ; dy+=m_gridStep){ double vy=toView(QPointF(0,dy)).y(); if(vy>height()) break;
        int k=qRound(dy/m_gridStep); pen(sub<=1 || k%sub==0); g.drawLine(QPointF(0,vy),QPointF(width(),vy)); }
}

// "Nice" tick interval: 1/2/5 × 10^k just above the requested doc-space value.
static double niceStep(double v) {
    if (v<=0) return 1;
    double e=std::floor(std::log10(v)), b=v/std::pow(10,e);
    double n = b<1.5?1 : b<3?2 : b<7?5 : 10;
    return n*std::pow(10,e);
}

// View-x of a paragraph-indent marker on the horizontal ruler, for a single selected (axis-aligned) text frame.
bool Canvas::indentMarkerX(int which, double& viewX) const {
    if(m_sel.size()!=1) return false;
    const Shape& s=m_doc.shapes[m_sel[0]];
    if(s.type!=ShapeType::Text || !s.textFrame) return false;
    if(qAbs(s.transform.m12())>1e-6 || qAbs(s.transform.m21())>1e-6) return false;   // rotated/sheared → no ruler markers
    const double localx = which==0 ? s.rect.left()+s.leftIndent+s.firstLineIndent   // markers live in the frame's LOCAL space
                        : which==1 ? s.rect.left()+s.leftIndent
                        :            s.rect.right()-s.rightIndent;
    viewX = toView(s.transform.map(QPointF(localx,0))).x();
    return viewX>=kRuler;
}
void Canvas::drawRulers(QPainter& g) {
    if (!m_showRulers) return;
    const int W=width(), H=height();
    const QColor bg(34,36,42), tick(120,125,135), txt(150,155,165), curCol(74,144,226);
    g.setRenderHint(QPainter::Antialiasing,false);
    g.setPen(Qt::NoPen); g.setBrush(bg);
    g.drawRect(QRectF(0,0,W,kRuler)); g.drawRect(QRectF(0,0,kRuler,H));

    const double uf=unitFactor(m_units);
    double majorPx=64, unitStep=niceStep((majorPx/m_zoom)/uf), docStep=unitStep*uf, pxStep=docStep*m_zoom;
    QFont f=g.font(); f.setPixelSize(8); g.setFont(f);
    // Labels are relative to the document's ruler origin (0,0 by default; user-settable zero point).
    auto label=[&](double docPos,double org){ double v=(docPos-org)/uf; return QString::number(v, (m_units==Units::Pixels?'f':'g'), (m_units==Units::Pixels?0:6)); };

    // horizontal ruler
    double dx0=toDoc(QPointF(kRuler,0)).x();
    double first=std::ceil(dx0/docStep)*docStep;
    for (double dx=first; toView(QPointF(dx,0)).x()<W; dx+=docStep) {
        double vx=toView(QPointF(dx,0)).x(); if(vx<kRuler) continue;
        g.setPen(QPen(tick,1)); g.drawLine(QPointF(vx,kRuler-5),QPointF(vx,kRuler));
        for(int m=1;m<5;++m){ double mx=vx+pxStep*m/5.0; if(mx<W) g.drawLine(QPointF(mx,kRuler-2.5),QPointF(mx,kRuler)); }
        g.setPen(txt); g.drawText(QPointF(vx+2, kRuler-7), label(dx, m_doc.rulerOrigin.x()));
    }
    // vertical ruler (numbers rotated)
    double dy0=toDoc(QPointF(0,kRuler)).y();
    double firstY=std::ceil(dy0/docStep)*docStep;
    for (double dy=firstY; toView(QPointF(0,dy)).y()<H; dy+=docStep) {
        double vy=toView(QPointF(0,dy)).y(); if(vy<kRuler) continue;
        g.setPen(QPen(tick,1)); g.drawLine(QPointF(kRuler-5,vy),QPointF(kRuler,vy));
        for(int m=1;m<5;++m){ double my=vy+pxStep*m/5.0; if(my<H) g.drawLine(QPointF(kRuler-2.5,my),QPointF(kRuler,my)); }
        g.save(); g.translate(kRuler-7, vy-2); g.rotate(-90); g.setPen(txt); g.drawText(QPointF(0,0), label(dy, m_doc.rulerOrigin.y())); g.restore();
    }
    // cursor position markers
    g.setPen(QPen(curCol,1));
    double cvx=toView(m_curDocPt).x(), cvy=toView(m_curDocPt).y();
    if(cvx>kRuler && cvx<W) g.drawLine(QPointF(cvx,0),QPointF(cvx,kRuler));
    if(cvy>kRuler && cvy<H) g.drawLine(QPointF(0,cvy),QPointF(kRuler,cvy));
    // Paragraph indent markers on the horizontal ruler (single selected text frame): first-line (top,
    // pointing down), left (bottom, pointing up), right (bottom, pointing up). Drag to set the indents.
    { double fx,lx,rx;
      g.setPen(QPen(QColor(60,60,60),1)); g.setBrush(QColor(255,255,255));
      if(indentMarkerX(0,fx)){ QPolygonF t; t<<QPointF(fx,1)<<QPointF(fx-4,7)<<QPointF(fx+4,7); g.drawPolygon(t); }        // first-line ▽
      if(indentMarkerX(1,lx)){ QPolygonF t; t<<QPointF(lx,kRuler-1)<<QPointF(lx-4,kRuler-7)<<QPointF(lx+4,kRuler-7); g.drawPolygon(t); }   // left △
      if(indentMarkerX(2,rx)){ QPolygonF t; t<<QPointF(rx,kRuler-1)<<QPointF(rx-4,kRuler-7)<<QPointF(rx+4,kRuler-7); g.drawPolygon(t); } } // right △

    // corner box with the current unit suffix
    g.setPen(Qt::NoPen); g.setBrush(bg.darker(115)); g.drawRect(QRectF(0,0,kRuler,kRuler));
    g.setPen(txt); g.drawText(QRectF(0,0,kRuler,kRuler),Qt::AlignCenter,unitSuffix(m_units));
}

QPointF Canvas::snapAdjust(const QRectF& bb, QVector<double>& gX, QVector<double>& gY) const {
    if (!m_snap) return QPointF(0,0);
    const double thr = 6.0/m_zoom;                 // doc-space snap distance
    QVector<double> xs, ys;
    auto add=[&](const QRectF& r){ xs<<r.left()<<r.center().x()<<r.right(); ys<<r.top()<<r.center().y()<<r.bottom(); };
    add(QRectF(QPointF(0,0), m_doc.pageSize));      // page edges + centre
    // Snap the moving bbox to other objects' edges/centres. m_sel is a QVector ⇒ contains() is O(M), so a plain
    // scan here would be O(N·M) and freeze a heavy doc during a drag — use a QSet for O(1) membership, and skip
    // object snapping entirely past kSnapObjectCandidateCap objects (page/guides/grid still snap).
    if (m_doc.shapes.size() <= kSnapObjectCandidateCap) {
        QSet<int> selSet(m_sel.constBegin(), m_sel.constEnd());
        for (int i=0;i<m_doc.shapes.size();++i){ if(selSet.contains(i)) continue; add(m_doc.shapes[i].docBounds()); }
    }
    const double sx[3]={bb.left(),bb.center().x(),bb.right()};
    const double sy[3]={bb.top(),bb.center().y(),bb.bottom()};
    if (m_snapGrid && m_gridStep>1) {               // snap bbox edges to the on-screen grid
        for(double s: sx) xs<<std::round(s/m_gridStep)*m_gridStep;
        for(double s: sy) ys<<std::round(s/m_gridStep)*m_gridStep;
    }
    for(double gx: m_guideVpos) xs<<gx;              // snap to persistent user guides
    for(double gy: m_guideHpos) ys<<gy;
    double bdx=thr, bdy=thr, adjx=0, adjy=0, gx=0, gy=0; bool hx=false, hy=false;
    for(double c: xs) for(double s: sx){ double dd=c-s; if(std::fabs(dd)<std::fabs(bdx)){ bdx=dd; adjx=dd; gx=c; hx=true; } }
    for(double c: ys) for(double s: sy){ double dd=c-s; if(std::fabs(dd)<std::fabs(bdy)){ bdy=dd; adjy=dd; gy=c; hy=true; } }
    if(hx) gX<<gx; if(hy) gY<<gy;
    return QPointF(hx?adjx:0, hy?adjy:0);
}

void Canvas::drawSelection(QPainter& g) {
    if (m_sel.isEmpty() || (m_tool!=Tool::Select && m_tool!=Tool::Node)) {
        if (m_rubber){ g.setPen(QPen(kAccent,1,Qt::DashLine)); g.setBrush(QColor(70,150,245,30)); g.drawRect(m_rubberRect);} return; }
    g.setRenderHint(QPainter::Antialiasing);
    if (m_tool==Tool::Node){          // the Node tool draws its own bezier-handle overlay; also its marquee box
        if (m_rubber){ g.setPen(QPen(kAccent,1,Qt::DashLine)); g.setBrush(QColor(70,150,245,30)); g.drawRect(m_rubberRect); }
        return; }
    // Select-into-group: draw the entered group's dashed boundary + the sub-selected child's outline & handles.
    if (inGroup()) {
        const Shape* c=enteredContainer();
        if (c) { const QTransform cx=containerXf();
            ensureSelOutlineCache();                                               // cached container silhouette (see header note)
            g.setPen(QPen(QColor(150,160,180),1,Qt::DashLine)); g.setBrush(Qt::NoBrush);
            if (m_selOutlineDoc.elementCount() <= kDragOutlineStrokeCap)
                g.drawPath(viewXf().map(m_selOutlineDoc));                          // "you are inside this group"
            g.setPen(QPen(kAccent,1.4)); g.setBrush(Qt::NoBrush);
            for (int ci:m_subSel){ if(ci<0||ci>=c->children.size())continue;
                g.drawPath(viewXf().map((cx*c->children[ci].transform).map(c->children[ci].localPath()))); }
        }
        if (m_rubber){ g.setPen(QPen(kAccent,1,Qt::DashLine)); g.setBrush(QColor(70,150,245,30)); g.drawRect(m_rubberRect); }
        QVector<QPointF> pts=handlePoints(); if(pts.isEmpty()) return;              // nothing sub-selected yet
        QRectF b=m_selBoundsDoc; QRectF vr(toView(b.topLeft()),toView(b.bottomRight()));
        g.setPen(QPen(kAccent,1,Qt::SolidLine)); g.setBrush(Qt::NoBrush); g.drawRect(vr);
        if (m_rotateMode) {
            for (int i=0;i<8;++i){ const bool corner=(i==0||i==2||i==4||i==6); g.setPen(QPen(kAccent,1.4)); g.setBrush(corner?QColor(255,170,40):QColor(110,200,120));
                if(corner) g.drawEllipse(pts[i],5,5);
                else { QPolygonF dia; dia<<pts[i]+QPointF(0,-5)<<pts[i]+QPointF(5,0)<<pts[i]+QPointF(0,5)<<pts[i]+QPointF(-5,0); g.drawPolygon(dia); } }
            const QPointF pv=toView(rotPivotDoc());
            g.setPen(QPen(kAccent,1.4)); g.setBrush(Qt::white); g.drawEllipse(pv,5,5);
            g.setPen(QPen(kAccent,1.2)); g.drawLine(pv+QPointF(-7,0),pv+QPointF(7,0)); g.drawLine(pv+QPointF(0,-7),pv+QPointF(0,7));
        } else {
            g.drawLine(QPointF(vr.center().x(),vr.top()), pts[8]);
            for (int i=0;i<8;++i){ g.setBrush(kHandle); g.setPen(QPen(kAccent,1.4)); g.drawRect(QRectF(pts[i].x()-4,pts[i].y()-4,8,8)); }
            g.setBrush(kAccent); g.setPen(QPen(Qt::white,1.4)); g.drawEllipse(pts[8],5,5);
        }
        return;
    }
    // outline each selected shape, then the bbox + handles. During a whole-selection affine drag the proxy has
    // cached the silhouette at drag start (doc space); remap it by the shared delta M and take its bounding rect,
    // instead of rebuilding each selected shape's path — which, for a group, walks every child EVERY frame (it was
    // rebuilt 3× per frame: outline + selBoundsDoc for the box + selBoundsDoc again for the handles).
    g.setPen(QPen(kAccent,1.2)); g.setBrush(Qt::NoBrush);
    QRectF vr;
    if (m_dragProxyActive && !m_dragSelOutline.isEmpty()) {
        QVector<Shape*> tg = selTargets();
        const QTransform M = tg.isEmpty() ? QTransform() : (m_dragProxyBaseXf.inverted() * tg[0]->transform);
        const QPainterPath viewOutline = (M * viewXf()).map(m_dragSelOutline);   // transform points only — no rasterisation
        vr = viewOutline.boundingRect();
        // Stroking the full silhouette each frame is the real cost (AA over every child edge). Keep it for a light
        // selection; for a heavy one skip it — the bounding box + handles convey the selection and the moving proxy
        // already shows the shapes. The exact silhouette returns the instant the drag ends.
        if (m_dragSelOutline.elementCount() <= kDragOutlineStrokeCap) g.drawPath(viewOutline);
    } else {
        ensureSelOutlineCache();   // build the silhouette + bounds once (cached), not per-frame — see the header note
        if (m_selOutlineDoc.elementCount() <= kDragOutlineStrokeCap) g.drawPath(viewXf().map(m_selOutlineDoc));
        if (m_selBoundsDoc.isNull()) return; vr = QRectF(toView(m_selBoundsDoc.topLeft()),toView(m_selBoundsDoc.bottomRight()));
    }
    const QVector<QPointF> pts=handlePointsForRect(vr);
    g.setPen(QPen(kAccent,1,Qt::SolidLine)); g.setBrush(Qt::NoBrush); g.drawRect(vr);
    if (m_rotateMode) {   // rotate/skew handles: corners rotate (orange circles), mid-edges skew (green diamonds), + draggable pivot
        for (int i=0;i<8;++i){ const bool corner=(i==0||i==2||i==4||i==6); g.setPen(QPen(kAccent,1.4)); g.setBrush(corner?QColor(255,170,40):QColor(110,200,120));
            if(corner) g.drawEllipse(pts[i],5,5);
            else { QPolygonF dia; dia<<pts[i]+QPointF(0,-5)<<pts[i]+QPointF(5,0)<<pts[i]+QPointF(0,5)<<pts[i]+QPointF(-5,0); g.drawPolygon(dia); } }
        const QPointF pv = (m_dragProxyActive && !m_rotPivotSet) ? vr.center() : toView(rotPivotDoc());   // cached: avoid selBoundsDoc rebuild
        g.setPen(QPen(kAccent,1.4)); g.setBrush(Qt::white); g.drawEllipse(pv,5,5);
        g.setPen(QPen(kAccent,1.2)); g.drawLine(pv+QPointF(-7,0),pv+QPointF(7,0)); g.drawLine(pv+QPointF(0,-7),pv+QPointF(0,7));
    } else {
        g.drawLine(QPointF(vr.center().x(),vr.top()), pts[8]);   // rotate stem
        for (int i=0;i<8;++i){ g.setBrush(kHandle); g.setPen(QPen(kAccent,1.4)); g.drawRect(QRectF(pts[i].x()-4,pts[i].y()-4,8,8)); }
        g.setBrush(kAccent); g.setPen(QPen(Qt::white,1.4)); g.drawEllipse(pts[8],5,5);
    }
    // SmartShape parameter handle (drag horizontally to change count/teeth/points)
    QPointF sh; if (smartHandle(sh)){
        g.setPen(QPen(QColor(70,150,245),1)); g.drawLine(QPointF(vr.right(),vr.top()), sh);
        QColor sc(255,170,40); g.setBrush(sc); g.setPen(QPen(Qt::white,1.4));
        QPolygonF dia; dia<<sh+QPointF(0,-6)<<sh+QPointF(6,0)<<sh+QPointF(0,6)<<sh+QPointF(-6,0); g.drawPolygon(dia);
    }
    // SmartShape 2nd-parameter handle (drag vertically to change depth/thickness/inner-radius)
    QPointF sh2; if (smartHandle2(sh2)){
        g.setPen(QPen(QColor(70,150,245),1)); g.drawLine(QPointF(vr.right(),vr.bottom()), sh2);
        g.setBrush(QColor(80,200,150)); g.setPen(QPen(Qt::white,1.4));   // teal square = shape parameter
        g.drawRect(QRectF(sh2.x()-5.5, sh2.y()-5.5, 11, 11));
    }
    // Rounded-corner handle (single Rect): drag along the top edge to set the corner radius.
    QPointF cr; if (cornerRadHandle(cr)){
        g.setBrush(QColor(255,170,40)); g.setPen(QPen(Qt::white,1.4)); g.drawEllipse(cr,4.5,4.5);
    }
    // Text-on-curve start handle: an orange chevron on the baseline; drag along the path to slide the text.
    QPointF tph; if (textPathStartHandle(tph)){
        g.setBrush(QColor(255,170,40)); g.setPen(QPen(Qt::white,1.4));
        QPolygonF tri; tri<<tph+QPointF(-6,-6)<<tph+QPointF(2,0)<<tph+QPointF(-6,6); g.drawPolygon(tri);
    }
    // Drop-cap resize handle: box at the drop cap's bottom-right; drag down to enlarge (more lines).
    QPointF dch; if (dropCapHandle(dch)){
        const QRectF cap=m_doc.shapes[m_sel[0]].dropCapRectLocal();
        g.setPen(QPen(QColor(70,150,245),1,Qt::DashLine)); g.setBrush(Qt::NoBrush);
        g.drawRect(QRectF(toView(m_doc.shapes[m_sel[0]].transform.map(cap.topLeft())), dch).normalized());
        g.setBrush(QColor(80,200,150)); g.setPen(QPen(Qt::white,1.4)); g.drawRect(QRectF(dch.x()-5,dch.y()-5,10,10));
    }
}

void Canvas::drawToolOverlay(QPainter& g) {
    g.setRenderHint(QPainter::Antialiasing);
    // Shadow tool: show the offset grip (drag the object to cast the shadow) + the blur handle.
    if (m_tool==Tool::Shadow && m_sel.size()==1) {
        const Shape& s=m_doc.shapes[m_sel[0]];
        if (s.shadow.on) {
            const QRectF b=s.docBounds();
            const QPointF c0=toView(b.center()), c1=toView(b.center()+s.shadow.offset);
            g.setPen(QPen(kAccent,1.2,Qt::DashLine)); g.setBrush(Qt::NoBrush); g.drawLine(c0,c1);
            g.setBrush(QColor(45,111,247)); g.setPen(QPen(Qt::white,1.4)); g.drawEllipse(c1,5,5);
            QPointF bh; if (shadowBlurHandle(bh)){
                g.setBrush(QColor(255,170,40)); g.setPen(QPen(Qt::white,1.4));
                QPolygonF dia; dia<<bh+QPointF(0,-5)<<bh+QPointF(5,0)<<bh+QPointF(0,5)<<bh+QPointF(-5,0); g.drawPolygon(dia);
            }
            QPointF rh; if (shadowRakeHandle(rh)){   // FLOOR shadow: teal tip handle = rake lean + length
                const QPointF ba=toView(QPointF(b.center().x()+s.shadow.offset.x(), b.bottom()));   // baseline anchor
                g.setPen(QPen(QColor(60,200,190),1.0,Qt::DashLine)); g.setBrush(Qt::NoBrush); g.drawLine(ba, rh);
                g.setBrush(QColor(60,200,190)); g.setPen(QPen(Qt::white,1.4)); g.drawRect(QRectF(rh.x()-4.5,rh.y()-4.5,9,9));
            }
            QPointF cs; if (shadowColorSwatch(cs)){   // shadow colour swatch (double-click / drop a colour to recolour)
                const QRectF sw(cs.x()-6.5, cs.y()-6.5, 13, 13);
                g.setPen(Qt::NoPen); g.setBrush(Qt::white); g.drawRect(sw);          // white backing so the alpha reads
                g.setBrush(s.shadow.color); g.drawRect(sw);
                g.setPen(QPen(Qt::white,1.4)); g.setBrush(Qt::NoBrush); g.drawRect(sw);
            }
        }
    }
    // Blend tool: the control line between the two ends + the orange step handle; drag-to-blend preview line.
    if (m_tool==Tool::Blend) {
        QPointF a,b;
        if (blendControlEnds(a,b)) {           // a selected LIVE blend group
            g.setPen(QPen(kAccent,1.2,Qt::DashLine)); g.setBrush(Qt::NoBrush); g.drawLine(a,b);
            QPointF sh; if (blendStepHandle(sh)) {
                g.setBrush(QColor(255,170,40)); g.setPen(QPen(Qt::white,1.4)); g.drawEllipse(sh,5,5);
            }
        }
        if (m_dragging && m_blendFromShape>=0 && m_blendFromShape<(int)m_doc.shapes.size()) {   // dragging from an object → preview line
            const QPointF from=toView(m_doc.shapes[m_blendFromShape].docBounds().center());
            g.setPen(QPen(kAccent,1.4,Qt::DotLine)); g.setBrush(Qt::NoBrush); g.drawLine(from, toView(m_curDocPt));
        }
    }
    // Contour / Bevel / Feather tools: the scalar handle past the shape's bottom-right corner.
    if ((m_tool==Tool::Contour||m_tool==Tool::Bevel||m_tool==Tool::Feather) && m_sel.size()==1) {
        QPointF eh;
        if (effectHandle(eh)) {
            const QRectF b=m_doc.shapes[m_sel[0]].docBounds();
            g.setPen(QPen(kAccent,1.0,Qt::DashLine)); g.setBrush(Qt::NoBrush); g.drawLine(toView(b.bottomRight()), eh);
            g.setBrush(QColor(255,170,40)); g.setPen(QPen(Qt::white,1.4));
            QPolygonF dia; dia<<eh+QPointF(0,-5)<<eh+QPointF(5,0)<<eh+QPointF(0,5)<<eh+QPointF(-5,0); g.drawPolygon(dia);
        }
        QPointF lh;
        if (m_tool==Tool::Bevel && bevelLightHandle(lh)) {   // yellow "sun" = bevel light azimuth
            const QPointF ctr=toView(m_doc.shapes[m_sel[0]].docBounds().center());
            g.setPen(QPen(QColor(255,214,80),1.0,Qt::DotLine)); g.setBrush(Qt::NoBrush); g.drawLine(ctr, lh);
            g.setBrush(QColor(255,214,80)); g.setPen(QPen(Qt::white,1.4)); g.drawEllipse(lh,5.5,5.5);
            g.setPen(QPen(QColor(255,214,80),1.4));
            for(int k=0;k<8;++k){ const double a=k*M_PI/4.0; const QPointF u(std::cos(a),std::sin(a)); g.drawLine(lh+u*7.0, lh+u*10.0); }
        }
        QPointF ccs; if (m_tool==Tool::Contour && contourColorSwatch(ccs)){   // contour colour swatch (double-click / drop a colour to recolour)
            const Shape& s=m_doc.shapes[m_sel[0]];
            const QRectF sw(ccs.x()-6.5, ccs.y()-6.5, 13, 13);
            g.setPen(Qt::NoPen); g.setBrush(Qt::white); g.drawRect(sw);          // white backing so the alpha reads
            g.setBrush(s.contour.color); g.drawRect(sw);
            g.setPen(QPen(Qt::white,1.4)); g.setBrush(Qt::NoBrush); g.drawRect(sw);
        }
        QPointF c2s; if (m_tool==Tool::Contour && contourColor2Swatch(c2s)){   // OUTER graduation colour (double-click); hollow dashed = unset
            const Shape& s=m_doc.shapes[m_sel[0]];
            const QRectF sw(c2s.x()-6.5, c2s.y()-6.5, 13, 13);
            if (s.contour.color2.isValid()){
                g.setPen(Qt::NoPen); g.setBrush(Qt::white); g.drawRect(sw);
                g.setBrush(s.contour.color2); g.drawRect(sw);
                g.setPen(QPen(Qt::white,1.4)); g.setBrush(Qt::NoBrush); g.drawRect(sw);
            } else {
                g.setPen(QPen(QColor(200,200,210),1.0,Qt::DashLine)); g.setBrush(Qt::NoBrush); g.drawRect(sw);   // "+ outer colour"
            }
        }
        QPointF bcs; if (m_tool==Tool::Bevel && bevelColorSwatch(bcs)){   // bevel light-colour swatch (double-click / drop a colour to recolour)
            const Shape& s=m_doc.shapes[m_sel[0]];
            const QRectF sw(bcs.x()-6.5, bcs.y()-6.5, 13, 13);
            g.setPen(Qt::NoPen); g.setBrush(Qt::white); g.drawRect(sw);          // white backing (opaque swatch)
            g.setBrush(s.bevel.lightColor); g.drawRect(sw);
            g.setPen(QPen(Qt::white,1.4)); g.setBrush(Qt::NoBrush); g.drawRect(sw);
        }
    }
    // 3D Extrude tool: the vanishing-point handle (distance = depth, direction = angle) + the light handle.
    if (m_tool==Tool::Extrude && m_sel.size()==1) {
        const QPointF ctr=toView(m_doc.shapes[m_sel[0]].docBounds().center());
        QPointF eh;
        if (extrudeHandle(eh)) {
            g.setPen(QPen(kAccent,1.2,Qt::DashLine)); g.setBrush(Qt::NoBrush); g.drawLine(ctr, eh);
            g.setBrush(QColor(255,170,40)); g.setPen(QPen(Qt::white,1.4)); g.drawEllipse(eh,5,5);
        }
        QPointF lh;
        if (extrudeLightHandle(lh)) {   // a yellow "sun" orbiting the centre = light azimuth
            g.setPen(QPen(QColor(255,214,80),1.0,Qt::DotLine)); g.setBrush(Qt::NoBrush); g.drawLine(ctr, lh);
            g.setBrush(QColor(255,214,80)); g.setPen(QPen(Qt::white,1.4)); g.drawEllipse(lh,5.5,5.5);
            g.setPen(QPen(QColor(255,214,80),1.4));
            for(int k=0;k<8;++k){ const double a=k*M_PI/4.0; const QPointF u(std::cos(a),std::sin(a));
                g.drawLine(lh+u*7.0, lh+u*10.0); }   // little rays
        }
        QPointF ecs; if (extrudeColorSwatch(ecs)){   // extrude wall-colour swatch (double-click / drop a colour to recolour)
            const Shape& s=m_doc.shapes[m_sel[0]];
            const QColor wc = s.extrude.color.isValid() ? s.extrude.color : s.fill.representativeColor();  // show the effective wall colour
            const QRectF sw(ecs.x()-6.5, ecs.y()-6.5, 13, 13);
            g.setPen(Qt::NoPen); g.setBrush(Qt::white); g.drawRect(sw);          // white backing (opaque swatch)
            g.setBrush(wc); g.drawRect(sw);
            g.setPen(QPen(Qt::white,1.4)); g.setBrush(Qt::NoBrush); g.drawRect(sw);
        }
    }
    // Ellipse tool: pie/arc/segment rim handles (only when the ellipse is swept).
    if (m_tool==Tool::Ellipse && m_sel.size()==1 && !m_dragging) {
        QPointF h; const QPointF ctr=toView(m_doc.shapes[m_sel[0]].rect.center());   // note: rect centre is local; approx guide only
        auto rim=[&](bool end,QColor col){ QPointF p; if(ellipseRimHandle(end,p)){
            g.setPen(QPen(col,1.0,Qt::DotLine)); g.setBrush(Qt::NoBrush);
            g.setBrush(col); g.setPen(QPen(Qt::white,1.4)); g.drawEllipse(p,5,5); } };
        (void)ctr; (void)h; rim(false, QColor(45,111,247)); rim(true, QColor(255,120,40));
    }
    // Polygon/QuickShape tool: edge-curvature handle + star inner-vertex depth handle.
    if (m_tool==Tool::Polygon && m_sel.size()==1 && !m_dragging) {
        QPointF cv; if (polyCurvHandle(cv)){ g.setBrush(QColor(160,90,240)); g.setPen(QPen(Qt::white,1.4)); g.drawEllipse(cv,5,5); }
        QPointF sd; if (starDepthHandle(sd)){ g.setBrush(QColor(255,214,80)); g.setPen(QPen(Qt::white,1.4));
            QPolygonF dia; dia<<sd+QPointF(0,-5)<<sd+QPointF(5,0)<<sd+QPointF(0,5)<<sd+QPointF(-5,0); g.drawPolygon(dia); }
    }
    // live preview for the shape-creation tools
    if (m_dragging && (m_tool==Tool::Rect||m_tool==Tool::Ellipse||m_tool==Tool::Polygon||m_tool==Tool::Line)) {
        const QPointF far = !m_createConstrain ? m_curDocPt
                          : (m_tool==Tool::Line ? constrainLineAngle(m_pressDocPt,m_curDocPt) : constrainSquareCorner(m_pressDocPt,m_curDocPt));
        QRectF r=QRectF(m_pressDocPt,far).normalized();
        Shape s=styledShape();
        if(m_tool==Tool::Rect){ s.type=ShapeType::Rect; s.rect=QRectF(0,0,r.width(),r.height()); s.transform=QTransform::fromTranslate(r.x(),r.y()); }
        else if(m_tool==Tool::Ellipse){ s.type=ShapeType::Ellipse; s.rect=QRectF(0,0,r.width(),r.height()); s.transform=QTransform::fromTranslate(r.x(),r.y()); }
        else if(m_tool==Tool::Polygon){ s.type=ShapeType::Polygon; s.polySides=m_polySides; s.polyStar=m_polyStar; s.rect=QRectF(0,0,r.width(),r.height()); s.transform=QTransform::fromTranslate(r.x(),r.y()); }
        else { s.type=ShapeType::Path; QPainterPath p; p.moveTo(m_pressDocPt); p.lineTo(far); s.path=p; s.fill.type=FillType::None; }
        g.save(); g.setTransform(viewXf(), true); g.setRenderHint(QPainter::Antialiasing,true);
        paintShape(g, s); g.restore();
    }
    if (m_dragging && m_tool==Tool::Table) {
        QRectF r=QRectF(m_pressDocPt,m_curDocPt).normalized();
        g.save(); g.setTransform(viewXf(), true); g.setRenderHint(QPainter::Antialiasing,false);
        QPen pen(kAccent, 1.0/qMax(0.001,m_zoom)); g.setPen(pen); g.setBrush(QColor(45,111,247,26));
        g.drawRect(r);
        for(int c=1;c<m_tableCols;++c){ double x=r.left()+r.width()*c/m_tableCols; g.drawLine(QPointF(x,r.top()),QPointF(x,r.bottom())); }
        for(int rr=1;rr<m_tableRows;++rr){ double y=r.top()+r.height()*rr/m_tableRows; g.drawLine(QPointF(r.left(),y),QPointF(r.right(),y)); }
        g.restore();
    }
    // Selected cell range highlight (Table tool)
    if (m_tool==Tool::Table && m_tableShape>=0 && m_tableShape<m_doc.shapes.size() && m_cellBR>=0) {
        const Shape& ts=m_doc.shapes[m_tableShape]; const Table& t=ts.table;
        QRect sel=tableSelRange();
        if (t.valid() && !sel.isNull()) {
            g.save(); g.setTransform(viewXf()*ts.transform, true);
            QRectF cr(t.colX(sel.left()), t.rowY(sel.top()),
                      t.colX(sel.right()+1)-t.colX(sel.left()), t.rowY(sel.bottom()+1)-t.rowY(sel.top()));
            g.setPen(QPen(kAccent, 1.4/qMax(0.001,m_zoom)));
            g.setBrush(QColor(45,111,247,40)); g.drawRect(cr);
            g.restore();
        }
    }
    if (m_dragging && m_tool==Tool::Region) {
        g.save(); g.setTransform(viewXf(), true); g.setRenderHint(QPainter::Antialiasing,true);
        QPen pen(kAccent, 1.4/qMax(0.001,m_zoom)); pen.setStyle(Qt::DashLine); g.setPen(pen); g.setBrush(QColor(45,111,247,26));
        if (m_regionFreehand) {
            if (m_freePts.size()>=2){ QPainterPath p; p.moveTo(m_freePts[0]); for(int i=1;i<m_freePts.size();++i) p.lineTo(m_freePts[i]); g.drawPath(p); }
        } else {
            QRectF r=QRectF(m_pressDocPt,m_curDocPt).normalized();
            if(m_regionEllipse) g.drawEllipse(r); else g.drawRect(r);
        }
        g.restore();
    }
    if (m_tool==Tool::Region && m_regionMagnetic && !m_lassoAnchors.isEmpty()) {
        // magnetic-lasso anchors: guide polyline (straight preview) + dots at each click
        QPen pen(kAccent, 1.4/qMax(0.001,m_zoom)); pen.setStyle(Qt::DashLine);
        g.save(); g.setTransform(viewXf(), true); g.setRenderHint(QPainter::Antialiasing,true);
        g.setBrush(Qt::NoBrush); g.setPen(pen);
        if(m_lassoAnchors.size()>=2){ QPainterPath p; p.moveTo(m_lassoAnchors[0]); for(int i=1;i<m_lassoAnchors.size();++i) p.lineTo(m_lassoAnchors[i]); g.drawPath(p); }
        const double rr=3.0/qMax(0.001,m_zoom); g.setPen(Qt::NoPen); g.setBrush(kAccent);
        for(const QPointF& a: m_lassoAnchors) g.drawEllipse(a, rr, rr);
        g.restore();
    }
    if (m_freePts.size()>=2 && m_tool==Tool::Freehand) {
        QPainterPath p; p.moveTo(toView(m_freePts[0])); for(int i=1;i<m_freePts.size();++i) p.lineTo(toView(m_freePts[i]));
        QPen pen(m_stroke.color, qMax(1.0,m_stroke.width*m_zoom)); pen.setCapStyle(Qt::RoundCap); pen.setJoinStyle(Qt::RoundJoin);
        g.setPen(pen); g.setBrush(Qt::NoBrush); g.drawPath(p);
    }
    if (m_penActive && !m_penAnchors.isEmpty()) {
        // Committed bezier so far, plus a live preview segment to the cursor.
        QPainterPath path=buildPenPath(false);
        QPointF last=m_penAnchors.last(), lastH=m_penHandles.last();
        QPointF cur=m_curDocPt;
        if (!m_penHandleDrag)
            path.cubicTo(lastH, cur, cur);   // preview from last anchor to cursor (corner end)
        g.setPen(QPen(kAccent,1.6)); g.setBrush(Qt::NoBrush);
        g.drawPath(viewXf().map(path));
        // Handle being dragged on the current anchor (symmetric in/out).
        if (m_penHandleDrag) {
            QPointF a=toView(last), ho=toView(lastH), hi=toView(last*2.0-lastH);
            g.setPen(QPen(QColor(120,170,255),1.1)); g.drawLine(hi,ho);
            g.setBrush(QColor(120,170,255)); g.setPen(Qt::NoPen);
            g.drawEllipse(ho,3.5,3.5); g.drawEllipse(hi,3.5,3.5);
        }
        // Anchor squares; the first is highlighted so the user sees where closing snaps.
        for(int k=0;k<m_penAnchors.size();++k){ QPointF v=toView(m_penAnchors[k]);
            bool first=(k==0);
            bool nearClose = first && m_penAnchors.size()>2 && QLineF(toView(cur),v).length()<9;
            g.setBrush(nearClose?QColor(120,255,170):Qt::white);
            g.setPen(QPen(kAccent,1.3)); g.drawRect(QRectF(v.x()-3.2,v.y()-3.2,6.4,6.4)); }
    }
    if (m_tool==Tool::Node && m_nodeShape>=0 && m_nodeShape<m_doc.shapes.size()) {
        const QTransform xf=nodeXf();   // group*child when node-editing a child inside a group
        auto V=[&](QPointF lp){ return toView(xf.map(lp)); };
        const QColor hCol(120,170,255);
        g.setPen(QPen(kAccent,1.0)); g.setBrush(Qt::NoBrush);
        if (Shape* tgt=nodeTargetShape()) g.drawPath(viewXf().map(xf.map(tgt->localPath())));
        // control handle stems + dots
        for (const auto& sp : m_nodes) for (const auto& nd : sp.nodes) {
            auto stem=[&](QPointF h){ g.setPen(QPen(hCol,1.0)); g.drawLine(V(nd.anchor),V(h));
                g.setPen(Qt::NoPen); g.setBrush(hCol); g.drawEllipse(V(h),3.2,3.2); };
            if(nd.hasIn)  stem(nd.in);
            if(nd.hasOut) stem(nd.out);
        }
        // anchors on top: filled diamond = smooth, hollow square = cusp; multi-selected nodes get an amber ring
        for (int si=0; si<m_nodes.size(); ++si) for (int ni=0; ni<m_nodes[si].nodes.size(); ++ni) {
            const PathNode& nd=m_nodes[si].nodes[ni]; QPointF a=V(nd.anchor);
            if (m_nodeSelSet.contains(qMakePair(si,ni))){ g.setPen(QPen(QColor(255,168,40),2.0)); g.setBrush(Qt::NoBrush); g.drawEllipse(a,7,7); }
            g.setPen(QPen(kAccent,1.4));
            if(nd.symmetric){ g.setBrush(kAccent);   // symmetric: filled diamond
                QPolygonF dia; dia<<QPointF(a.x(),a.y()-4.2)<<QPointF(a.x()+4.2,a.y())<<QPointF(a.x(),a.y()+4.2)<<QPointF(a.x()-4.2,a.y());
                g.drawPolygon(dia); }
            else if(nd.smooth){ g.setBrush(kAccent); g.drawEllipse(a,4.0,4.0); }   // smooth: filled circle
            else { g.setBrush(Qt::white); g.drawRect(QRectF(a.x()-3.4,a.y()-3.4,6.8,6.8)); }   // cusp: hollow square
        }
    }
    if (m_tool==Tool::Mould && m_mouldShape>=0) {
        if(m_mouldMode==1){
            // Envelope: draw the four cubic-bezier edges + their control tethers/handles.
            QPainterPath cage; cage.moveTo(toView(m_mouldCorners[0]));
            for(int i=0;i<4;++i) cage.cubicTo(toView(m_mouldCtrl[2*i]),toView(m_mouldCtrl[2*i+1]),toView(m_mouldCorners[(i+1)&3]));
            cage.closeSubpath();
            g.setPen(QPen(kAccent,1.2,Qt::DashLine)); g.setBrush(Qt::NoBrush); g.drawPath(cage);
            g.setPen(QPen(QColor(kAccent.red(),kAccent.green(),kAccent.blue(),150),0.9));
            for(int i=0;i<4;++i){ QPointF a=toView(m_mouldCorners[i]), b=toView(m_mouldCorners[(i+1)&3]);
                g.drawLine(a,toView(m_mouldCtrl[2*i])); g.drawLine(b,toView(m_mouldCtrl[2*i+1])); }
            for(int i=0;i<8;++i){ QPointF v=toView(m_mouldCtrl[i]); g.setBrush(kAccent); g.setPen(QPen(Qt::white,1.0)); g.drawEllipse(v,3.4,3.4); }
        } else {
            QPolygonF q; for(int i=0;i<4;++i) q<<toView(m_mouldCorners[i]);
            g.setPen(QPen(kAccent,1.2,Qt::DashLine)); g.setBrush(Qt::NoBrush); g.drawPolygon(q);
        }
        for(int i=0;i<4;++i){ QPointF v=toView(m_mouldCorners[i]); g.setBrush(Qt::white); g.setPen(QPen(kAccent,1.6)); g.drawEllipse(v,5,5); }
    }
    // In-canvas text editing overlay: selection highlight + blinking caret (mapped through the shape's transform).
    if (m_textEdit>=0 && m_textEdit<m_doc.shapes.size() && m_doc.shapes[m_textEdit].type==ShapeType::Text) {
        const Shape& s=m_doc.shapes[m_textEdit]; const QTransform xf=s.transform;
        const double asc=s.textAscent(), lh=s.textLineH();
        auto caretQuad=[&](int idx)->QPair<QPointF,QPointF>{ QPointF lp=s.textCaretLocal(idx);
            return { xf.map(QPointF(lp.x(), lp.y()-asc)), xf.map(QPointF(lp.x(), lp.y()-asc+lh)) }; };
        // Selection: highlight each character's cell between the two caret positions.
        if (m_caretAnchor>=0 && m_caretAnchor!=m_caret){
            int a=qMin(m_caret,m_caretAnchor), b=qMax(m_caret,m_caretAnchor);
            g.setPen(Qt::NoPen); g.setBrush(QColor(70,150,245,70));
            for(int i=a;i<b;++i){ if(i<s.text.size() && s.text[i]=='\n') continue;
                // Skip the diagonal cell where char i and i+1 are on different (wrapped) lines. Compare the
                // LOCAL baselines (transform-independent) so a rotated/sheared frame doesn't drop same-line cells.
                if(qAbs(s.textCaretLocal(i).y()-s.textCaretLocal(i+1).y())>0.5) continue;
                auto c0=caretQuad(i), c1=caretQuad(i+1);
                QPolygonF cell; cell<<toView(c0.first)<<toView(c1.first)<<toView(c1.second)<<toView(c0.second);
                g.drawPolygon(cell); }
        }
        if (m_caretBlink){ auto c=caretQuad(m_caret);
            g.setPen(QPen(QColor(20,24,32), 1.4)); g.drawLine(toView(c.first), toView(c.second)); }
        // Interactive spell-check: red wavy underline beneath each misspelled word (G-D).
        if (m_spellLive){
            g.setPen(QPen(QColor(220,45,45), 1.3));
            for(const auto& r: misspelledRanges(s.text)){ int a=r.first, b=r.second;
                const QPointF la=s.textCaretLocal(a), lb=s.textCaretLocal(b);
                if(qAbs(la.y()-lb.y())>0.5) continue;                 // word wrapped across lines — skip
                const double y=la.y()+2.0;                            // just below the baseline
                QPolygonF wig; bool up=true;
                for(double x=la.x(); x<lb.x(); x+=3.0){ wig<<toView(xf.map(QPointF(x, y+(up?-1.5:1.5)))); up=!up; }
                wig<<toView(xf.map(QPointF(lb.x(), y)));
                if(wig.size()>=2) g.drawPolyline(wig); }
        }
    }
    if ((m_tool==Tool::Fill||m_tool==Tool::Transparency) && !m_sel.isEmpty()) {
        Shape& s=m_doc.shapes[m_sel.first()];
        QPointF p0,p1,p2; bool draw=false, useP2=false;
        if (m_tool==Tool::Fill && s.fill.type!=FillType::Solid && s.fill.type!=FillType::None){
            p0=s.fill.p0; p1=s.fill.p1; p2=s.fill.p2; draw=true;
            const FillType ft=s.fill.type; useP2=(ft==FillType::Radial||ft==FillType::Mesh||ft==FillType::Square||ft==FillType::Bitmap);
        } else if (m_tool==Tool::Transparency && m_transpMode==0 && s.transp.geom!=4 && s.transp.isGraduated()){
            p0=s.transp.p0; p1=s.transp.p1; p2=s.transp.p2; draw=true; useP2=(s.transp.geom==2);
        }
        if (draw){
            QPointF a=toView(s.transform.map(p0)), b=toView(s.transform.map(p1)), m=(a+b)/2;
            g.setPen(QPen(m_gradHandleColor,1.6)); g.drawLine(a,b);
            if (useP2){ QPointF c2=toView(s.transform.map(p2));   // second axis in green
                g.setPen(QPen(QColor(120,230,140),1.5)); g.drawLine(a,c2);
                g.setBrush(QColor(120,230,140)); g.setPen(QPen(Qt::white,1.3)); g.drawEllipse(c2,5,5); }
            g.setPen(QPen(m_gradHandleColor,1.5)); g.setBrush(Qt::white); g.drawEllipse(a,5,5); g.drawEllipse(b,5,5);
            QPolygonF md; md<<m+QPointF(0,-5)<<m+QPointF(5,0)<<m+QPointF(0,5)<<m+QPointF(-5,0);   // midpoint = translate
            g.setBrush(m_gradHandleColor); g.setPen(QPen(Qt::white,1.3)); g.drawPolygon(md);
        }
    }
    // Mesh-gradient corner colour swatches (Fill tool on a Mesh fill) — double-click a swatch to recolour it.
    if (m_tool==Tool::Fill && m_sel.size()==1 && m_doc.shapes[m_sel.first()].fill.type==FillType::Mesh) {
        const Shape& s=m_doc.shapes[m_sel.first()]; QVector<QPointF> lc=meshCornerLocals(s);
        const QVector<QColor>& mc=s.fill.meshColors;
        auto colAt=[&](int i)->QColor{ if(i<mc.size()) return mc[i];
            if(mc.size()>=3){ QColor a=mc[0],b=mc[1],c=mc[2]; return QColor(qBound(0,b.red()+c.red()-a.red(),255),qBound(0,b.green()+c.green()-a.green(),255),qBound(0,b.blue()+c.blue()-a.blue(),255)); }
            return mc.isEmpty()?QColor(Qt::black):mc.first(); };
        for (int i=0;i<4;++i){ QPointF v=toView(s.transform.map(lc[i])); QRectF sw(v.x()-6,v.y()-6,12,12);
            g.setBrush(colAt(i)); g.setPen(QPen(i==m_meshCorner?kAccent:Qt::white, i==m_meshCorner?2.2:1.4)); g.drawRect(sw); }
    }
    if (m_tool==Tool::Clone || (m_tool==Tool::Transparency && m_transpMode==1)) {
        const double rv = (m_tool==Tool::Clone? m_cloneNib : m_maskNib)*m_zoom;   // brush ring at the cursor
        g.setBrush(Qt::NoBrush); g.setPen(QPen(QColor(255,255,255,200),1)); g.drawEllipse(toView(m_curDocPt), rv, rv);
        g.setPen(QPen(QColor(0,0,0,120),1,Qt::DotLine)); g.drawEllipse(toView(m_curDocPt), rv, rv);
        if (m_cloneHasSrc && m_cloneShape>=0 && m_cloneShape<m_doc.shapes.size()
            && m_doc.shapes[m_cloneShape].fill.type==FillType::Bitmap && !m_doc.shapes[m_cloneShape].fill.image.isNull()){   // source crosshair
            const Shape& s=m_doc.shapes[m_cloneShape];
            const double W=qMax(1,s.fill.image.width()), H=qMax(1,s.fill.image.height());
            const QPointF ax=(s.fill.p1-s.fill.p0)/W, ay=(s.fill.p0-s.fill.p2)/H;
            QTransform tex(ax.x(),ax.y(),ay.x(),ay.y(),s.fill.p2.x(),s.fill.p2.y());   // image -> local
            QPointF srcLocal=tex.map(m_cloneSrcImg), srcV=toView(s.transform.map(srcLocal));
            g.setPen(QPen(QColor(255,90,90),1.5)); g.drawLine(srcV+QPointF(-7,0),srcV+QPointF(7,0)); g.drawLine(srcV+QPointF(0,-7),srcV+QPointF(0,7));
            g.setBrush(Qt::NoBrush); g.drawEllipse(srcV,4,4);
        }
    }
}

#include "canvas_input.inc"
#include "canvas_io.inc"
#include "canvas_radial.inc"
