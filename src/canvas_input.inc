// canvas_input.inc — mouse/keyboard handling for every tool (included by canvas.cpp).

// Build a smooth Catmull-Rom -> cubic-bezier path through sampled points.
static QPainterPath smoothPath(const QVector<QPointF>& pts, double tol=2.5) {
    QPainterPath p; if (pts.size()<2) return p;
    // Decimation so tiny jitters don't create knots. `tol` is the freehand smoothing/fit tolerance:
    // larger = fewer sample points = smoother, simpler curve. Default 2.5 keeps the classic behaviour.
    QVector<QPointF> q; q<<pts.first(); for (const QPointF& v:pts) if (QLineF(q.last(),v).length()>tol) q<<v; if(q.last()!=pts.last()) q<<pts.last();
    if (q.size()<3){ p.moveTo(q.first()); for(int i=1;i<q.size();++i) p.lineTo(q[i]); return p; }
    p.moveTo(q[0]);
    for (int i=0;i<q.size()-1;++i){
        QPointF p0=q[qMax(0,i-1)], p1=q[i], p2=q[i+1], p3=q[qMin(q.size()-1,i+2)];
        QPointF c1=p1+(p2-p0)/6.0, c2=p2-(p3-p1)/6.0;
        p.cubicTo(c1,c2,p2);
    }
    return p;
}

QPainterPath Canvas::smoothFreehand(const QVector<QPointF>& pts, double tol){ return smoothPath(pts, tol); }

void Canvas::commitNewShape(Shape s, const QString& label) {
    pushUndo(label);
    s.layer = m_activeLayer;   // new objects go on the active layer
    m_doc.shapes.push_back(s);
    m_sel = { m_doc.shapes.size()-1 };
    emit documentChanged(); emit selectionChanged(); update();
}

void Canvas::mousePressEvent(QMouseEvent* e) {
    setFocus();
    // A press starts a real interaction (select / drag / draw) — the interactive zoom-preview is over, so drop it
    // and let this frame render crisply (the settle timer would eventually do this, but a press must not wait for it).
    if(m_previewZoom){ m_previewZoom=false; if(m_zoomSettleTimer) m_zoomSettleTimer->stop(); }
    // A mouse press CANCELS the pie (releasing Ctrl/Alt is the sole commit gesture) and falls through, so a
    // Ctrl- or Alt-modified drag (constrain / snap-bypass) still begins normally instead of the press being
    // swallowed as a destructive wedge click.
    if(m_radialActive) radialDisarm();
    if(m_radialTimer && m_radialTimer->isActive()) m_radialTimer->stop();
    const QPointF w = e->position();
    const QPointF d = toDoc(w);
    m_pressView = m_lastView = w; m_pressDocPt = d; m_curDocPt = d;
    m_pressMoved = false; m_moveUndoPending = false;   // reset per-press; a plain click stays a no-op (no re-raster, no undo)

    if (m_cropMode) {   // interactive crop: grab a handle (or ignore); all left-clicks stay in crop mode
        if (e->button()==Qt::LeftButton) m_cropHandle = cropHandleAt(w);
        return;
    }
    if (m_straightenMode) {   // grab the nearest reference-line endpoint (else drag the whole line)
        if (e->button()==Qt::LeftButton){
            const double da=QLineF(w,toView(m_straightenA)).length(), db=QLineF(w,toView(m_straightenB)).length();
            m_straightenGrab = (da<12 && da<=db) ? 0 : (db<12) ? 1 : 2;
        }
        return;
    }
    if (e->button()==Qt::MiddleButton || (e->button()==Qt::LeftButton && m_tool==Tool::Pan)) {
        m_panning = true; m_panStart = m_origin; setCursor(Qt::ClosedHandCursor); return;
    }
    if (e->button()!=Qt::LeftButton) return;
    // In-canvas text editing: a click while editing repositions the caret inside the shape (drag =
    // select), or — clicked outside — commits the edit and falls through to normal handling.
    if (editingText()){
        const bool inside = m_doc.shapes[m_textEdit].docBounds().adjusted(-6,-6,6,6).contains(d);
        if (inside && (m_tool==Tool::Text || m_tool==Tool::Select)){
            setCaretAtDocPoint(d, e->modifiers()&Qt::ShiftModifier); m_textDragging=true; return; }
        endTextEdit();
    }
    // Ruler guides: drag a NEW guide out of the ruler strip, or grab an existing guide line to move/delete it.
    // Grab a paragraph-indent marker on the horizontal ruler (single selected text frame) before guide creation.
    if (m_showRulers && w.y()<kRuler && w.x()>=kRuler && m_sel.size()==1
        && m_doc.shapes[m_sel[0]].type==ShapeType::Text && m_doc.shapes[m_sel[0]].textFrame) {
        int hit=-1; double bestdx=6.0;
        for(int wi=0; wi<3; ++wi){ double xv; if(indentMarkerX(wi,xv)){ double dx=qAbs(xv-w.x());
            const bool yok = (wi==0) ? (w.y()<kRuler*0.55) : (w.y()>=kRuler*0.45);   // first-line on top, left/right on bottom
            if(dx<bestdx && yok){ bestdx=dx; hit=wi; } } }
        if(hit>=0){ m_indentDrag=hit; pushUndo("Indent"); return; }
    }
    if (m_showRulers && (w.x()<kRuler || w.y()<kRuler)) {
        pushUndo("Guide");
        if (w.x()<kRuler){ m_guideDrag=GDNewV; m_guideVpos.push_back(d.x()); m_dragGuideIdx=m_guideVpos.size()-1; }
        else             { m_guideDrag=GDNewH; m_guideHpos.push_back(d.y()); m_dragGuideIdx=m_guideHpos.size()-1; }
        update(); return;
    }
    { const double tol=3.0;
      for(int i=0;i<m_guideHpos.size();++i) if(qAbs(toView(QPointF(0,m_guideHpos[i])).y()-w.y())<=tol){ pushUndo("Move guide"); m_guideDrag=GDMoveH; m_dragGuideIdx=i; return; }
      for(int i=0;i<m_guideVpos.size();++i) if(qAbs(toView(QPointF(m_guideVpos[i],0)).x()-w.x())<=tol){ pushUndo("Move guide"); m_guideDrag=GDMoveV; m_dragGuideIdx=i; return; } }

    switch (m_tool) {
    case Tool::Select: {
        Handle h = hitHandle(w);
        if (h==SmartParam && m_sel.size()==1) {
            m_activeHandle=SmartParam; m_dragging=true; m_dragStartBounds=selBoundsDoc();
            m_smartStartParam=m_doc.shapes[m_sel[0]].smartParam; pushUndo("SmartShape parameter"); return;
        }
        if (h==SmartParam2 && m_sel.size()==1) {   // 2nd handle: drag vertically to change the shape's depth/thickness
            m_activeHandle=SmartParam2; m_dragging=true; m_dragStartBounds=selBoundsDoc();
            const Shape& s=m_doc.shapes[m_sel[0]];
            m_smartStartParam2 = s.smartParam2>=0 ? s.smartParam2 : Shape::smartParam2Default(s.smartKind);
            pushUndo("SmartShape parameter"); return;
        }
        if (h==TextPathStart && m_sel.size()==1) {   // slide text-on-curve along its baseline path
            m_activeHandle=TextPathStart; m_dragging=true; pushUndo("Text start offset"); return;
        }
        if (h==DropCapSize && m_sel.size()==1) {     // drag the drop-cap corner to change its line span
            m_activeHandle=DropCapSize; m_dragging=true; pushUndo("Drop cap"); return;
        }
        if (h==CornerRad && m_sel.size()==1) {   // drag the top-edge handle to round the rectangle's corners
            m_activeHandle=CornerRad; m_dragging=true; pushUndo("Round corner"); return;
        }
        if (h==RotPivot){ m_activeHandle=RotPivot; m_dragging=true; return; }   // drag the pivot marker (no doc change → no undo)
        if (h!=HNone && !m_sel.isEmpty() && (!inGroup() || !m_subSel.isEmpty())) {
            m_activeHandle=h; m_dragging=true; m_dragStartBounds=selBoundsDoc();
            if(!inGroup() && m_sel.size()==1 && m_doc.shapes[m_sel[0]].type==ShapeType::Text) m_textStartRect=m_doc.shapes[m_sel[0]].rect;
            m_dragStartXf.clear(); for(Shape* s:selTargets()) m_dragStartXf.push_back(s->transform);
            m_dragStartWidth.clear(); for(Shape* s:selTargets()) m_dragStartWidth.push_back(s->stroke.width);   // "scale line widths"
            if (h==Rotate||h==SkewH||h==SkewV){ m_rotCenterActive=rotPivotDoc();
                if(h==Rotate){ m_rotating=true; m_rotStartAngle=std::atan2(d.y()-m_rotCenterActive.y(), d.x()-m_rotCenterActive.x()); } }
            pushUndo(h==Rotate?"Rotate":(h==SkewH||h==SkewV)?"Skew":"Resize"); return;
        }
        // Select-into-group: while inside a group, clicking picks a CHILD (no ungrouping); clicking outside the
        // group's bounds pops back out. m_sel keeps the group selected so menu/structural ops stay at the safe level.
        if (inGroup()) {
            Shape* c=enteredContainer();
            if (!c) { resetGroupEntry(); }
            else {
                const QTransform cx=containerXf();
                const int childHit=hitChildInGroup(*c, cx, d);
                const bool inside = cx.map(c->localPath()).boundingRect().adjusted(-4,-4,4,4).contains(d);
                if (childHit>=0) {
                    if (e->modifiers()&Qt::ShiftModifier){ if(m_subSel.contains(childHit)) m_subSel.removeAll(childHit); else m_subSel.push_back(childHit); }
                    else if (!m_subSel.contains(childHit)){ m_subSel={childHit}; m_rotateMode=false; m_rotPivotSet=false; }
                    m_dragging=true; m_dragStartXf.clear(); for(Shape* s:selTargets()) m_dragStartXf.push_back(s->transform);
                    m_dragStartWidth.clear(); for(Shape* s:selTargets()) m_dragStartWidth.push_back(s->stroke.width);
                    m_dragStartBounds=selBoundsDoc(); pushUndo("Move in group"); emit selectionChanged();
                } else if (!inside) { exitGroup(); }        // clicked outside the group → leave it
                else { m_subSel.clear(); emit selectionChanged(); }   // clicked empty space inside the group → deselect
            }
            update(); break;
        }
        // Alt+click selects the object BEHIND the current one (cycles down the overlapping z-stack).
        const bool altBehind = e->modifiers()&Qt::AltModifier;
        int hit = altBehind ? hitShapeBehind(d, m_sel.size()==1? m_sel.first() : -1) : hitShape(d);
        m_toggleRotCandidate=false;
        if (hit>=0) {
            const bool reclick = !altBehind && m_sel.size()==1 && m_sel.first()==hit && !(e->modifiers()&Qt::ShiftModifier);
            if (altBehind){ m_sel={hit}; m_rotateMode=false; m_rotPivotSet=false; }   // pick the object underneath, fresh single selection
            else if (e->modifiers()&Qt::ShiftModifier){ if(m_sel.contains(hit)) m_sel.removeAll(hit); else m_sel.push_back(hit); }
            else if (!m_sel.contains(hit)){ m_sel={hit}; m_rotateMode=false; m_rotPivotSet=false; }   // a new selection resets the handle mode
            m_toggleRotCandidate=reclick;   // re-clicking the same object (no drag) flips resize<->rotate/skew on release
            // expand to soft-group siblings (members select together)
            { QSet<int> grp; for(int i:m_sel){ int g=m_doc.shapes[i].softGroup; if(g>0) grp.insert(g); }
              if(!grp.isEmpty()) for(int i=0;i<m_doc.shapes.size();++i) if(m_doc.shapes[i].softGroup>0 && grp.contains(m_doc.shapes[i].softGroup) && !m_sel.contains(i)) m_sel.push_back(i); }
            m_dragging=true; m_dragStartXf.clear(); for(int i:m_sel) m_dragStartXf.push_back(m_doc.shapes[i].transform);
            m_dragStartBounds=selBoundsDoc();
            m_moveUndoPending=true;   // defer the "Move" snapshot to the first real move — a plain click must not serialise the whole doc
            emit selectionChanged();
        } else { m_sel.clear(); m_rubber=true; m_rubberRect=QRectF(w,w); emit selectionChanged(); }
        update(); break;
    }
    case Tool::Node: {
        // (Re)select the shape under edit, parsing its nodes the first time. If the hit is a group,
        // descend to the child path under the cursor so its nodes can be edited without ungrouping.
        int hit=hitShape(d);
        if(hit>=0){
            int child=-1;
            const Shape& hs=m_doc.shapes[hit];
            if(hs.type==ShapeType::Group){
                double best=1e18; const double tolDoc=8.0/qMax(m_zoom,1e-6);
                for(int ci=0; ci<hs.children.size(); ++ci){
                    const Shape& c=hs.children[ci];
                    if(c.type==ShapeType::Group) continue;              // only descend one level
                    const QPainterPath cp=(hs.transform*c.transform).map(c.localPath());
                    QPainterPathStroker st; st.setWidth(qMax(c.stroke.width,1.0)+tolDoc*2);
                    if(cp.contains(d) || st.createStroke(cp).contains(d)){
                        const double dd=QLineF(d, cp.boundingRect().center()).length();
                        if(dd<best){ best=dd; child=ci; } }
                }
            }
            if(hit!=m_nodeShape || child!=m_nodeChild){
                m_sel={hit}; emit selectionChanged();
                if(child>=0) beginNodeEditChild(hit,child); else beginNodeEdit(hit);
            }
        }
        else if(m_nodeShape<0 && !m_sel.isEmpty()) beginNodeEdit(m_sel.first());
        m_nodeSub=-1; m_nodeIndex=-1; m_nodePart=NPAnchor;
        if(m_nodeShape>=0){
            const QTransform xf=nodeXf();
            double best=9.0;  // pixels
            auto tryHit=[&](int si,int ni,NodePart part,QPointF local){
                double dist=QLineF(w, toView(xf.map(local))).length();
                if(dist<best){ best=dist; m_nodeSub=si; m_nodeIndex=ni; m_nodePart=part; } };
            // Handles take priority over anchors so you can grab a handle sitting near its node.
            for(int si=0;si<m_nodes.size();++si) for(int ni=0;ni<m_nodes[si].nodes.size();++ni){
                const PathNode& nd=m_nodes[si].nodes[ni];
                if(nd.hasIn)  tryHit(si,ni,NPIn,nd.in);
                if(nd.hasOut) tryHit(si,ni,NPOut,nd.out);
            }
            for(int si=0;si<m_nodes.size();++si) for(int ni=0;ni<m_nodes[si].nodes.size();++ni)
                tryHit(si,ni,NPAnchor,m_nodes[si].nodes[ni].anchor);
            if(m_nodeSub>=0){ pushUndo("Edit node"); m_dragging=true;
                // Maintain the multi-selection set: Shift toggles the anchor; a plain click on a node
                // NOT already selected resets the set to just that node (so a group can be dragged).
                if(m_nodePart==NPAnchor){ QPair<int,int> nk(m_nodeSub,m_nodeIndex);
                    if(e->modifiers()&Qt::ShiftModifier){ if(m_nodeSelSet.contains(nk)) m_nodeSelSet.removeAll(nk); else m_nodeSelSet.push_back(nk); }
                    else if(!m_nodeSelSet.contains(nk)){ m_nodeSelSet.clear(); m_nodeSelSet.push_back(nk); } }
            }
            else {
                // No node/handle grabbed: grab the nearest SEGMENT and drag it to reshape its curve
                // (Xara-style "pull the line to bend it"). Node insertion moves to double-click.
                // Sample each segment with the SAME (synthesized-for-straight) control basis the bend will
                // use, so the parameter bt corresponds to the point actually under the cursor.
                double bestSeg=6.0; int bs=-1,bi=-1; double bt=0;
                for(int si=0;si<m_nodes.size();++si){ const SubPath& sp=m_nodes[si]; int nn=sp.nodes.size();
                    int segs = sp.closed? nn : nn-1;
                    for(int i=0;i<segs;++i){ int jj=(i+1)%nn; const PathNode& A=sp.nodes[i]; const PathNode& B=sp.nodes[jj];
                        QPointF p0=A.anchor,p3=B.anchor;
                        QPointF p1=A.hasOut?A.out:p0+(p3-p0)/3.0, p2=B.hasIn?B.in:p3-(p3-p0)/3.0;
                        for(int k=0;k<=16;++k){ double t=k/16.0,u=1-t;
                            QPointF pt=p0*u*u*u+p1*3*u*u*t+p2*3*u*t*t+p3*t*t*t;
                            double dist=QLineF(w, toView(xf.map(pt))).length();
                            if(dist<bestSeg){ bestSeg=dist; bs=si; bi=i; bt=t; } } } }
                if(bs>=0){
                    const SubPath& sp=m_nodes[bs]; int nn=sp.nodes.size(); int jj=(bi+1)%nn;
                    const PathNode& A=sp.nodes[bi]; const PathNode& B=sp.nodes[jj]; const QPointF a=A.anchor, b=B.anchor;
                    m_bendSub=bs; m_bendIdx=bi; m_bendT=bt; m_bendActive=false;
                    m_bendOut0 = A.hasOut? A.out : a+(b-a)/3.0;
                    m_bendIn0  = B.hasIn ? B.in  : b-(b-a)/3.0;
                    const double u=1-bt; m_bendGrab = a*u*u*u + m_bendOut0*3*u*u*bt + m_bendIn0*3*u*bt*bt + b*bt*bt*bt;
                    m_dragging=true;   // undo is deferred to the first past-threshold move (a plain click is a no-op)
                }
                else { m_nodeSelSet.clear(); m_rubber=true; m_rubberRect=QRectF(w,w); }   // marquee-select nodes
            }
        }
        update(); break;
    }
    case Tool::Rect: { m_dragging=true; break; }   // build on move/release
    case Tool::Ellipse: {
        if(m_sel.size()==1 && m_doc.shapes[m_sel[0]].type==ShapeType::Ellipse){ QPointF h;   // grab a pie/arc rim handle
            if(ellipseRimHandle(false,h) && QLineF(w,h).length()<12){ m_activeHandle=EllipseStart; m_dragging=true; pushUndo("Ellipse angle"); break; }
            if(ellipseRimHandle(true ,h) && QLineF(w,h).length()<12){ m_activeHandle=EllipseEnd;   m_dragging=true; pushUndo("Ellipse angle"); break; } }
        m_dragging=true; break;   // draw on drag; a plain click (caught by the release size-guard) selects an existing ellipse
    }
    case Tool::Polygon: {
        if(m_sel.size()==1 && m_doc.shapes[m_sel[0]].type==ShapeType::Polygon){ QPointF h;   // grab a star-depth / curvature handle
            if(starDepthHandle(h) && QLineF(w,h).length()<12){ m_activeHandle=StarDepth; m_dragging=true; pushUndo("Star depth"); break; }
            if(polyCurvHandle(h)  && QLineF(w,h).length()<12){ m_activeHandle=PolyCurv;  m_dragging=true; pushUndo("Curvature"); break; } }
        m_dragging=true; break;   // draw on drag; a plain click (caught by the release size-guard) selects an existing polygon
    }
    case Tool::Region: {
        if(m_regionMagnetic){                      // magnetic lasso: click to drop anchors, double-click to close
            int hit=(m_lassoShape>=0)?m_lassoShape:hitShape(d);
            if(hit>=0 && m_doc.shapes[hit].fill.type==FillType::Bitmap){ m_lassoShape=hit; m_lassoAnchors<<d; m_sel={hit}; emit selectionChanged(); update(); }
            else emit statusMessage("Magnetic lasso: click on a photo to place anchor points.");
            m_dragging=false; break;
        }
        m_dragging=true; if(m_regionFreehand){ m_freePts.clear(); m_freePts<<d; } break;
    }
    case Tool::Table: {
        // Click on an existing table → select a cell (Shift extends the range, drag rubber-bands cells).
        // Click on empty pasteboard → drag out a new grid.
        int hit=hitShape(d);
        if (hit>=0 && m_doc.shapes[hit].type==ShapeType::Table){
            const Shape& ts=m_doc.shapes[hit]; const Table& tb=ts.table;
            const QPointF lp=ts.transform.inverted().map(d);
            const double px=QLineF(toView(ts.transform.map(QPointF(0,0))), toView(ts.transform.map(QPointF(10,0)))).length()/10.0;
            const double tolL=(px>1e-6? 5.0/px : 5.0);
            bool grabbed=false;
            { double x=0; for(int i=0;i<tb.cols()-1 && !grabbed;++i){ x+=tb.colW[i];   // drag an interior column divider
                if(qAbs(lp.x()-x)<tolL && lp.y()>=-tolL && lp.y()<=tb.height()+tolL){
                    m_tableDivShape=hit; m_tableColDivider=i; m_tableRowDivider=-1; m_tableDivStart=tb.colW[i]; m_dragging=true; pushUndo("Column width"); grabbed=true; } } }
            if(!grabbed){ double y=0; for(int j=0;j<tb.rows()-1 && !grabbed;++j){ y+=tb.rowH[j];   // or an interior row divider
                if(qAbs(lp.y()-y)<tolL && lp.x()>=-tolL && lp.x()<=tb.width()+tolL){
                    m_tableDivShape=hit; m_tableRowDivider=j; m_tableColDivider=-1; m_tableDivStart=tb.rowH[j]; m_dragging=true; pushUndo("Row height"); grabbed=true; } } }
            if(grabbed){ update(); break; }
            int r,c; if (tableCellAt(hit,d,r,c)){ tableSelectCell(hit,r,c, e->modifiers()&Qt::ShiftModifier); m_cellRangeDrag=true; }
            m_dragging=false;
        } else { clearTableSel(); emit selectionChanged(); m_dragging=true; }
        break;
    }
    case Tool::Line: { m_dragging=true; break; }
    case Tool::Pen: {
        if(!m_penActive){ m_penActive=true; m_penAnchors.clear(); m_penHandles.clear(); m_penResume=-1; m_penResumeBase=QPainterPath();
            int ri=-1; bool atStart=false;
            if(penHitEndpoint(d, ri, atStart)){
                // Resume an existing open path: extend from its end node (reverse it if the start was clicked).
                const Shape& rs=m_doc.shapes[ri]; QPainterPath base=rs.transform.map(rs.localPath());
                if(atStart) base=base.toReversed();
                m_penResume=ri; m_penResumeBase=base;
                const QPointF endNode(base.elementAt(base.elementCount()-1));
                m_penAnchors<<endNode; m_penHandles<<endNode; m_penHandleDrag=true;
            } else { m_penAnchors<<d; m_penHandles<<d; m_penHandleDrag=true; } }
        else {
            // Clicking the first anchor closes the path into a filled shape.
            if(m_penAnchors.size()>2 && QLineF(toView(d),toView(m_penAnchors[0])).length()<9){
                commitPen(true); return;
            }
            m_penAnchors<<d; m_penHandles<<d; m_penHandleDrag=true;
        }
        update(); break;
    }
    case Tool::Freehand: { m_dragging=true; m_freePts.clear(); m_freePts<<d; break; }
    case Tool::Text: {
        // Click existing text → edit it with the caret at the click; click empty canvas → new text object
        // with a live caret. (No more modal dialog — you type directly on the canvas.)
        int hit=hitShape(d);
        if(hit>=0 && m_doc.shapes[hit].type==ShapeType::Text && !Canvas::canInlineEdit(m_doc.shapes[hit])){
            // frames / text-on-curve / tabbed text: exact caret maths don't apply → modal editor
            bool ok=false; QString t=QInputDialog::getText(this,"Edit Text","Text:",QLineEdit::Normal,m_doc.shapes[hit].text,&ok);
            if(ok){ pushUndo("Edit text"); m_doc.shapes[hit].text=t; m_sel={hit}; emit documentChanged(); emit selectionChanged(); update(); }
        } else if(hit>=0 && m_doc.shapes[hit].type==ShapeType::Text){
            m_textEditIsNew=false;
            int ci=m_doc.shapes[hit].textIndexAtLocal(m_doc.shapes[hit].transform.inverted().map(d));
            beginTextEdit(hit, ci);
        } else {
            // Empty canvas: create point text NOW and start editing (so the caret has keyboard focus this
            // press — deferring to release let the InfoBar rebuild steal focus). If the user then DRAGS, the
            // move handler converts this same object into a sized wrapping frame.
            Shape s=styledShape(); s.type=ShapeType::Text; s.font=m_textFont; s.text=QString(); s.stroke.enabled=false; s.fill.type=FillType::Solid;
            s.transform=QTransform::fromTranslate(d.x(),d.y()); s.name="Text"; commitNewShape(s,"Text");
            m_textEditIsNew=true; beginTextEdit(m_sel.first(), 0);
            m_textDragStart=d; m_textDragPending=true; m_dragging=true;
        }
        break;
    }
    case Tool::Fill: {
        // Eyedropper: Alt+click samples the composited colour under the cursor → current fill + selection.
        if (e->modifiers()&Qt::AltModifier){ QColor c=sampleColorAt(d);
            if(c.isValid()){ Fill f=m_fill; f.type=FillType::Solid; f.color=c; setCurrentFill(f);
                if(!m_sel.isEmpty()) applyToSelection([&](Shape& s){ if(s.fill.type==FillType::None)s.fill.type=FillType::Solid; if(s.fill.type==FillType::Solid)s.fill.color=c; },"Eyedropper"); }
            update(); break; }
        // Re-edit an existing gradient: grab its start/end handle or midpoint (Xara's on-object fill arrow).
        if(!m_sel.isEmpty()){ Shape& s=m_doc.shapes[m_sel.first()];
            if(s.fill.type!=FillType::Solid && s.fill.type!=FillType::None){
                QPointF va=toView(s.transform.map(s.fill.p0)), vb=toView(s.transform.map(s.fill.p1)), vm=(va+vb)/2;
                const FillType ft=s.fill.type; const bool usesP2=(ft==FillType::Radial||ft==FillType::Mesh||ft==FillType::Square||ft==FillType::Bitmap);   // bitmap fill is a parallelogram: p2 = second (Y) axis
                const bool mesh=(ft==FillType::Mesh);
                if(mesh){ QPointF v3=toView(s.transform.map(s.fill.p1+s.fill.p2-s.fill.p0));   // 4th mesh corner: select for recolour (position derived)
                    if(QLineF(w,v3).length()<9){ m_meshCorner=3; emit selectionChanged(); update(); break; } }
                if(usesP2){ QPointF vc=toView(s.transform.map(s.fill.p2));
                    if(QLineF(w,vc).length()<9){ pushUndo("Gradient"); m_activeHandle=GradP2; m_dragging=true; if(mesh)m_meshCorner=2; update(); break; } }
                if(QLineF(w,va).length()<9){ pushUndo("Gradient"); m_activeHandle=GradP0; m_dragging=true; if(mesh)m_meshCorner=0; update(); break; }
                if(QLineF(w,vb).length()<9){ pushUndo("Gradient"); m_activeHandle=GradP1; m_dragging=true; if(mesh)m_meshCorner=1; update(); break; }
                if(QLineF(w,vm).length()<9){ pushUndo("Gradient"); m_activeHandle=GradMid; m_dragging=true;
                    m_gradStart0=s.fill.p0; m_gradStart1=s.fill.p1; m_gradStart2=s.fill.p2; update(); break; } } }
        int hit=hitShape(d); if(hit>=0){ m_sel={hit}; emit selectionChanged();
            Shape& s=m_doc.shapes[hit]; pushUndo("Gradient");
            if(s.fill.type==FillType::Solid||s.fill.type==FillType::None){ QColor c=s.fill.color.isValid()?s.fill.color:m_fill.color;
                s.fill.type=FillType::Linear; s.fill.stops={{0.0,c.lighter(140)},{1.0,c.darker(150)}}; }
            QPointF lp=s.transform.inverted().map(d); s.fill.p0=lp; s.fill.p1=lp; m_activeHandle=GradP1; m_dragging=true;
        }
        update(); break;
    }
    case Tool::Transparency: {
        if(m_transpMode==1){                                          // PAINT MASK mode
            int hit=hitShape(d); if(hit<0){ update(); break; }
            m_sel={hit}; emit selectionChanged();
            Shape& s=m_doc.shapes[hit];
            pushUndo(m_maskErase?"Paint mask":"Restore mask");        // maskBmp is JSON-snapshotted → undoable
            ensureMask(s);
            QPointF imgPt; if(!docToMaskPixel(hit,d,imgPt)){ update(); break; }
            m_maskShape=hit; m_maskStroke=true; m_maskLastImg=imgPt;
            maskDab(s, QPointF(qRound(imgPt.x()),qRound(imgPt.y())), m_maskErase?0.0:255.0);
            emit documentChanged(); emit selectionChanged(); update(); break; }
        // Re-edit an existing graduated transparency: grab its start/end/second-axis/midpoint handle.
        if(!m_sel.isEmpty()){ Shape& s=m_doc.shapes[m_sel.first()];
            if(s.transp.isGraduated()){
                QPointF va=toView(s.transform.map(s.transp.p0)), vb=toView(s.transform.map(s.transp.p1)), vm=(va+vb)/2;
                if(s.transp.geom==2){ QPointF vc=toView(s.transform.map(s.transp.p2));
                    if(QLineF(w,vc).length()<9){ pushUndo("Transparency"); m_activeHandle=GradP2; m_dragging=true; update(); break; } }
                if(QLineF(w,va).length()<9){ pushUndo("Transparency"); m_activeHandle=GradP0; m_dragging=true; update(); break; }
                if(QLineF(w,vb).length()<9){ pushUndo("Transparency"); m_activeHandle=GradP1; m_dragging=true; update(); break; }
                if(QLineF(w,vm).length()<9){ pushUndo("Transparency"); m_activeHandle=GradMid; m_dragging=true;
                    m_gradStart0=s.transp.p0; m_gradStart1=s.transp.p1; m_gradStart2=s.transp.p2; update(); break; } } }
        int hit=hitShape(d); if(hit>=0){ m_sel={hit}; emit selectionChanged();
            Shape& s=m_doc.shapes[hit]; pushUndo("Transparency");
            if(s.transp.type==0) s.transp.type=1;                 // Mix
            s.transp.geom=1; s.transp.t0=0; s.transp.t1=1;        // opaque -> transparent along the drag
            QPointF lp=s.transform.inverted().map(d); s.transp.p0=lp; s.transp.p1=lp; m_activeHandle=GradP1; m_dragging=true; }
        update(); break;
    }
    case Tool::Shadow: {
        // Colour swatch: a single click is a no-op (keeps the selection); double-click opens the picker.
        if(m_sel.size()==1){ QPointF cs; if(shadowColorSwatch(cs) && QLineF(w,cs).length()<9){ m_activeHandle=HNone; m_dragging=false; update(); break; } }
        // Floor-shadow rake/tip handle first, then the blur handle (both need a shadowed shape selected);
        // then drag the object to cast/move the shadow. A press without a drag toggles it (on release).
        if(m_sel.size()==1){ QPointF rh; if(shadowRakeHandle(rh) && QLineF(w,rh).length()<10){
            m_activeHandle=ShadowRake; m_dragging=true; m_shadowDidDrag=false; update(); break; } }   // undo deferred to first move
        if(m_sel.size()==1){ QPointF bh; if(shadowBlurHandle(bh) && QLineF(w,bh).length()<10){
            m_activeHandle=ShadowBlur; m_dragging=true; m_shadowDidDrag=false; pushUndo("Shadow blur"); update(); break; } }
        int hit=hitShape(d);
        if(hit>=0){ m_sel={hit}; m_activeHandle=ShadowOff; m_dragging=true; m_shadowDidDrag=false;
            m_shadowStartOffset=m_doc.shapes[hit].shadow.offset; emit selectionChanged(); }
        else { m_sel.clear(); m_activeHandle=HNone; emit selectionChanged(); }
        update(); break;
    }
    case Tool::Contour:{
        { QPointF cs; if(contourColorSwatch(cs) && QLineF(w,cs).length()<9){ m_activeHandle=HNone; m_dragging=false; update(); break; } }   // swatch: click no-op, dbl-click picks colour
        { QPointF cs; if(contourColor2Swatch(cs) && QLineF(w,cs).length()<9){ m_activeHandle=HNone; m_dragging=false; update(); break; } }  // outer-colour swatch: dbl-click sets color2
        { QPointF eh; if(effectHandle(eh) && QLineF(w,eh).length()<10){ m_activeHandle=EffectScalar; m_dragging=true; pushUndo("Contour width"); update(); break; } }
        int hit=hitShape(d); if(hit>=0){ m_sel={hit}; pushUndo("Contour"); Shape&s=m_doc.shapes[hit]; s.contour.on=!s.contour.on; if(s.contour.on&&s.contour.width==0.0)s.contour.width=8; emit selectionChanged(); emit documentChanged(); } update(); break; }
    case Tool::Bevel:  {
        { QPointF eh; if(effectHandle(eh) && QLineF(w,eh).length()<10){ m_activeHandle=EffectScalar; m_dragging=true; pushUndo("Bevel depth"); update(); break; } }
        { QPointF lh; if(bevelLightHandle(lh) && QLineF(w,lh).length()<10){ m_activeHandle=BevelLight; m_dragging=true; pushUndo("Bevel light"); update(); break; } }
        { QPointF cs; if(bevelColorSwatch(cs) && QLineF(w,cs).length()<9){ m_activeHandle=HNone; m_dragging=false; update(); break; } }   // after the drag handles (the 58px light orbit can overlap the swatch): swatch click no-op, dbl-click picks colour
        int hit=hitShape(d); if(hit>=0){ m_sel={hit}; pushUndo("Bevel"); Shape&s=m_doc.shapes[hit]; s.bevel.on=!s.bevel.on; if(s.bevel.on&&s.bevel.depth<=0)s.bevel.depth=7; emit selectionChanged(); emit documentChanged(); } update(); break; }
    case Tool::Extrude:{
        { QPointF eh; if(extrudeHandle(eh) && QLineF(w,eh).length()<10){ m_activeHandle=ExtrudeVec; m_dragging=true; pushUndo("Extrude"); update(); break; } }
        { QPointF lh; if(extrudeLightHandle(lh) && QLineF(w,lh).length()<10){ m_activeHandle=ExtrudeLight; m_dragging=true; pushUndo("Extrude light"); update(); break; } }
        { QPointF cs; if(extrudeColorSwatch(cs) && QLineF(w,cs).length()<9){ m_activeHandle=HNone; m_dragging=false; update(); break; } }   // after the drag handles (the 58px light orbit can overlap the swatch): swatch click no-op, dbl-click picks colour
        int hit=hitShape(d); if(hit>=0){ m_sel={hit}; pushUndo("Extrude"); Shape&s=m_doc.shapes[hit]; s.extrude.on=!s.extrude.on; if(s.extrude.on&&s.extrude.depth<=0)s.extrude.depth=28; emit selectionChanged(); emit documentChanged(); } update(); break; }
    case Tool::Feather:{
        { QPointF eh; if(effectHandle(eh) && QLineF(w,eh).length()<10){ m_activeHandle=EffectScalar; m_dragging=true; pushUndo("Feather"); update(); break; } }
        int hit=hitShape(d); if(hit>=0){ m_sel={hit}; emit selectionChanged(); } update(); break; }   // drag the corner handle out to feather
    case Tool::Eraser: { int hit=hitShape(d); if(hit>=0){
        // Soft eraser over a photo: paint a feathered alpha hole (stroke continues on drag).
        if(m_eraserSoft && m_doc.shapes[hit].fill.type==FillType::Bitmap && !m_doc.shapes[hit].fill.image.isNull()){
            pushUndo("Soft erase"); m_softEraseShape=hit; softEraseAt(hit,d); m_sel={hit};
            emit selectionChanged(); emit documentChanged(); update(); break; }
        pushUndo("Erase"); Shape s=m_doc.shapes[hit]; QPainterPath dp=Document::docPath(s);
        QPainterPath nib; nib.addEllipse(d, m_eraserNib, m_eraserNib); QPainterPath cut=dp.subtracted(nib);
        // bake the result into an editable Path shape in doc space (transform identity)
        Shape ns; ns.type=ShapeType::Path; ns.path=cut; ns.fill=s.fill; ns.stroke=s.stroke; ns.opacity=s.opacity; ns.blend=s.blend;
        ns.transp=s.transp; ns.fillRule=s.fillRule; ns.name=s.name; ns.transform=QTransform();
        m_doc.shapes[hit]=ns; m_sel={hit}; emit selectionChanged(); emit documentChanged(); } update(); break; }
    case Tool::Clone: {
        int hit=hitShape(d); QPointF imgPt;
        if (m_cloneMode==2){                                          // Red-eye: no source; de-red locally under the brush
            if(hit>=0 && docToImagePixel(hit,d,imgPt)){ pushUndo("Red-eye"); Shape& s=m_doc.shapes[hit];
                m_cloneShape=hit; m_cloneStroke=true; redeyeDab(s, QPointF(qRound(imgPt.x()),qRound(imgPt.y())));
                m_sel={hit}; emit documentChanged(); emit selectionChanged(); }
            else emit statusMessage("Click on a photo's red eye to remove it.");
            update(); break; }
        if (e->modifiers() & Qt::AltModifier) {                       // Alt-click: set the clone source
            if (hit>=0 && docToImagePixel(hit,d,imgPt)){ m_cloneShape=hit; m_cloneSrcImg=imgPt; m_cloneHasSrc=true;
                m_sel={hit}; emit selectionChanged(); emit statusMessage("Clone source set — now paint over the area to fix."); }
            else emit statusMessage("Alt-click on a photo (bitmap fill) to set the clone source.");
            update(); break; }
        if (!m_cloneHasSrc){ emit statusMessage("Alt-click a photo to set the clone source first."); update(); break; }
        if (m_cloneShape<0 || m_cloneShape>=m_doc.shapes.size() || !docToImagePixel(m_cloneShape,d,imgPt)){ update(); break; }
        pushUndo(m_cloneMode==1?"Heal":"Clone");                      // fill.image is JSON-snapshotted → whole stroke undoable
        Shape& s=m_doc.shapes[m_cloneShape];
        m_cloneSnap = s.fill.image.convertToFormat(QImage::Format_ARGB32);   // pre-stroke source (no self-smear)
        if(m_cloneSnap.isNull()){ resetCloneStroke(); emit statusMessage("Could not snapshot the photo for cloning."); update(); break; }
        s.fill.origImage = QImage();                                 // clone edits are permanent pixels; drop the stale photo-adjust source
        m_cloneOffset = m_cloneSrcImg - imgPt;                        // fixed source→dest offset for the stroke
        m_cloneStroke=true; m_cloneLastImg=imgPt;
        QPointF dp0(qRound(imgPt.x()),qRound(imgPt.y()));
        cloneDab(s, m_cloneSnap, dp0, dp0+m_cloneOffset);
        m_sel={m_cloneShape}; emit documentChanged(); update(); break; }
    case Tool::Blend:  {
        { QPointF sh; if(blendStepHandle(sh) && QLineF(w,sh).length()<10){   // drag the step handle of a selected live blend
            m_activeHandle=BlendStep; m_dragging=true; m_smartStartParam=m_doc.shapes[m_sel[0]].blendSteps; pushUndo("Blend steps"); update(); break; } }
        int hit=hitShape(d);
        if(hit>=0){ m_sel={hit}; m_blendFromShape=hit; m_dragging=true; emit selectionChanged(); }   // drag from here to another object to blend
        else { m_sel.clear(); m_blendFromShape=-1; emit selectionChanged(); }
        update(); break; }
    case Tool::Mould:  { if(m_mouldShape>=0){
            // Grab the CLOSEST handle within its radius (corners r<9, controls r<8) so control points
            // seeded near a corner stay grabbable when zoomed out (they'd otherwise be shadowed by the
            // corner's larger hit circle).
            int best=-1; double bestD=1e18;
            for(int i=0;i<4;++i){ double dd=QLineF(w,toView(m_mouldCorners[i])).length(); if(dd<9 && dd<bestD){ bestD=dd; best=i; } }
            if(m_mouldMode==1) for(int i=0;i<8;++i){ double dd=QLineF(w,toView(m_mouldCtrl[i])).length(); if(dd<8 && dd<bestD){ bestD=dd; best=4+i; } }
            if(best>=0){ m_mouldDrag=best; pushUndo(best<4?"Mould":"Envelope"); return; } }
        { int hit=hitShape(d); if(hit>=0){ m_sel={hit}; emit selectionChanged(); initMould(hit); } else { m_mouldShape=-1; } } update(); break; }
    case Tool::Zoom:   { m_rubber=true; break; }   // defer to release: click zooms, drag = marquee zoom to the rect
    case Tool::Pan:    { m_panning=true; m_panStart=m_origin; setCursor(Qt::ClosedHandCursor); break; }
    }
}

void Canvas::mouseMoveEvent(QMouseEvent* e) {
    if(m_radialActive){ radialHover(e->position()); return; }   // while the pie is up, the cursor only aims a wedge
    const QPointF w=e->position(); const QPointF d=toDoc(w); m_curDocPt=d;
    if (e->buttons() & Qt::LeftButton) m_pressMoved=true;   // an actual drag has begun → manipulatingContent() may now re-raster
    m_createConstrain = e->modifiers()&Qt::ControlModifier;   // Ctrl while drawing a new shape → square / angle-snap (live preview reads this)
    if (m_textDragging){ setCaretAtDocPoint(d, true); return; }   // drag inside edited text = extend selection
    emit mouseDoc(d);
    if (m_indentDrag>=0 && m_sel.size()==1) {   // drag a paragraph-indent marker on the ruler
        Shape& s=m_doc.shapes[m_sel[0]];
        if(s.type==ShapeType::Text && s.textFrame){ const QPointF lp=s.transform.inverted().map(d); const double W=s.rect.width();
            if(m_indentDrag==0)      s.firstLineIndent = qBound(-s.leftIndent, lp.x()-(s.rect.left()+s.leftIndent), qMax(0.0,W-s.leftIndent-s.rightIndent-4));
            else if(m_indentDrag==1) s.leftIndent      = qBound(0.0, lp.x()-s.rect.left(),  qMax(0.0,W-s.rightIndent-8));
            else                     s.rightIndent     = qBound(0.0, s.rect.right()-lp.x(),  qMax(0.0,W-s.leftIndent-8));
            emit documentChanged(); emit selectionChanged(); update();
        }
        return;
    }
    if (m_guideDrag!=GDNone) {   // live-drag a ruler guide
        if ((m_guideDrag==GDNewH||m_guideDrag==GDMoveH) && m_dragGuideIdx>=0 && m_dragGuideIdx<m_guideHpos.size()) m_guideHpos[m_dragGuideIdx]=d.y();
        else if ((m_guideDrag==GDNewV||m_guideDrag==GDMoveV) && m_dragGuideIdx>=0 && m_dragGuideIdx<m_guideVpos.size()) m_guideVpos[m_dragGuideIdx]=d.x();
        update(); return;
    }
    if (m_cropMode) {   // drag a crop handle, clamped to the photo's bounds
        if (m_cropHandle>=0 && (e->buttons()&Qt::LeftButton) && m_cropShape>=0 && m_cropShape<m_doc.shapes.size()){
            const QRectF db=m_doc.shapes[m_cropShape].docBounds();
            const double x=qBound(db.left(),d.x(),db.right()), y=qBound(db.top(),d.y(),db.bottom());
            QRectF& r=m_cropRectDoc;
            switch(m_cropHandle){ case 0: r.setTopLeft({x,y}); break; case 1: r.setTop(y); break; case 2: r.setTopRight({x,y}); break;
                case 3: r.setRight(x); break; case 4: r.setBottomRight({x,y}); break; case 5: r.setBottom(y); break;
                case 6: r.setBottomLeft({x,y}); break; case 7: r.setLeft(x); break; }
            m_cropRectDoc=r.normalized(); update();
        } else { setCursor(cropHandleAt(w)>=0?Qt::SizeAllCursor:Qt::CrossCursor); }
        return;
    }
    if (m_straightenMode) {   // drag the reference line to lie along something that should be level
        if (m_straightenGrab>=0 && (e->buttons()&Qt::LeftButton)){
            const QPointF dd=d-m_pressDocPt;
            if(m_straightenGrab==0) m_straightenA=d;
            else if(m_straightenGrab==1) m_straightenB=d;
            else { m_straightenA+=dd; m_straightenB+=dd; m_pressDocPt=d; }   // whole-line move
            update();
        }
        return;
    }
    // Pen: while the button is held after placing an anchor, drag its symmetric bezier handle.
    if (m_penHandleDrag && (e->buttons()&Qt::LeftButton)) { m_penHandles.last()=d; update(); return; }
    if (m_panning){ m_origin = m_panStart + (w - m_pressView); update(); return; }
    if (!(e->buttons()&Qt::LeftButton) && !m_dragging) {
        if (m_tool==Tool::Select){ Handle h=hitHandle(w); setCursor(h==Rotate?Qt::CrossCursor:(h!=HNone?Qt::SizeAllCursor:Qt::ArrowCursor)); }
        if (m_penActive){ update(); }
        if (m_tool==Tool::Clone || (m_tool==Tool::Transparency && m_transpMode==1)){ update(); }   // keep the brush ring tracking on hover
        return;
    }

    switch (m_tool) {
    case Tool::Select: {
        if (m_rubber){ m_rubberRect=QRectF(m_pressView,w).normalized(); update(); return; }
        if (m_activeHandle==SmartParam && m_sel.size()==1){
            Shape& s=m_doc.shapes[m_sel[0]];
            if(s.type==ShapeType::Smart){ double step=qMax(6.0, m_dragStartBounds.width()/16.0);
                double np=qBound(2.0, m_smartStartParam + (d.x()-m_pressDocPt.x())/step, 64.0);
                if(int(np+0.5)!=int(s.smartParam+0.5)){ s.smartParam=double(qRound(np)); emit documentChanged(); emit selectionChanged(); }
                update(); }
            return; }
        if (m_activeHandle==SmartParam2 && m_sel.size()==1){
            Shape& s=m_doc.shapes[m_sel[0]];
            if(s.type==ShapeType::Smart){ double hgt=qMax(20.0, m_dragStartBounds.height());
                double np=qBound(0.0, m_smartStartParam2 + (d.y()-m_pressDocPt.y())/hgt, 1.0);
                if(s.smartParam2<0.0 || qAbs(np-s.smartParam2)>1e-4){ s.smartParam2=np; emit documentChanged(); emit selectionChanged(); }
                update(); }
            return; }
        if (m_activeHandle==TextPathStart && m_sel.size()==1){   // project the drag onto the baseline → new start offset
            Shape& s=m_doc.shapes[m_sel[0]];
            if(s.type==ShapeType::Text && !s.textPath.isEmpty()){
                const QPointF lp=s.transform.inverted().map(d);        // drag point in the text's local space
                const double len=s.textPath.length();
                double best=1e18, bestLen=0; const int N=200;          // sample the path by arc length (Qt percent IS arc-length)
                for(int i=0;i<=N;++i){ double t=double(i)/N; QPointF pp=s.textPath.pointAtPercent(t);
                    double dd=QLineF(pp,lp).length(); if(dd<best){ best=dd; bestLen=t*len; } }
                double nd=qBound(0.0, bestLen, qMax(0.0,len-1.0));
                if(qAbs(nd-s.textPathOffset)>1e-4){ s.textPathOffset=nd; emit documentChanged(); emit selectionChanged(); }
                update(); }
            return; }
        if (m_activeHandle==DropCapSize && m_sel.size()==1){   // vertical drag → drop-cap line span
            Shape& s=m_doc.shapes[m_sel[0]];
            if(s.type==ShapeType::Text){ double lineH=s.textLineH(); if(lineH>1e-6){
                const double ly=s.transform.inverted().map(d).y();
                int lines=qBound(1, int(ly/lineH + 0.5), 8);
                if(lines!=s.dropCap && !(lines<=1 && s.dropCap<=1)){ s.dropCap=(lines<=1?0:lines);
                    if(lines>1){ s.textFrame=true; if(s.rect.width()<20) s.rect.setWidth(320); if(s.rect.height()<20) s.rect.setHeight(200); }
                    emit documentChanged(); emit selectionChanged(); } }
                update(); }
            return; }
        if (m_activeHandle==RotPivot){ m_rotPivotSet=true; m_rotPivotDoc=d; update(); return; }   // move the rotation/skew centre
        if (m_activeHandle==Rotate){ const QPointF c=m_rotCenterActive; double a=std::atan2(d.y()-c.y(),d.x()-c.x())-m_rotStartAngle;
            if(e->modifiers()&Qt::ControlModifier){ const double step=M_PI/12.0; a=std::round(a/step)*step; }   // Ctrl = snap to 15°
            { auto tg=selTargets(); for(int k=0;k<tg.size()&&k<m_dragStartXf.size();++k){ QTransform t; t.translate(c.x(),c.y()); t.rotateRadians(a); t.translate(-c.x(),-c.y()); tg[k]->transform=m_dragStartXf[k]*t; } }
            emit documentChanged(); update(); return; }
        if (m_activeHandle==SkewH || m_activeHandle==SkewV){ const QPointF c=m_rotCenterActive; double shx=0,shy=0;
            if(m_activeHandle==SkewH) shx=(d.x()-m_pressDocPt.x())/qMax(1.0,m_dragStartBounds.height());   // top/bottom edge → horizontal shear
            else                      shy=(d.y()-m_pressDocPt.y())/qMax(1.0,m_dragStartBounds.width());    // left/right edge → vertical shear
            { auto tg=selTargets(); for(int k=0;k<tg.size()&&k<m_dragStartXf.size();++k){ QTransform t; t.translate(c.x(),c.y()); t.shear(shx,shy); t.translate(-c.x(),-c.y()); tg[k]->transform=m_dragStartXf[k]*t; } }
            emit documentChanged(); update(); return; }
        if (m_activeHandle==CornerRad && m_sel.size()==1){   // round the rectangle by dragging along its top edge
            Shape& s=m_doc.shapes[m_sel[0]];
            if(s.type==ShapeType::Rect){
                const QPointF lp=s.transform.inverted().map(d);   // cursor in the rect's local space
                const double maxR=qMin(s.rect.width(),s.rect.height())/2.0;
                const double nr=qBound(0.0, lp.x()-s.rect.left(), maxR);
                if(qAbs(nr-s.cornerRadius)>1e-6){ s.cornerRadius=nr; emit documentChanged(); emit selectionChanged(); }
            }
            update(); return; }
        // Text FRAME: dragging a SIDE handle reflows the frame (edits rect w/h at constant font size),
        // instead of scaling the glyphs. Corner handles still scale. (Fixes "text scales like a graphic".)
        if (m_sel.size()==1 && (m_activeHandle==T||m_activeHandle==R||m_activeHandle==B||m_activeHandle==L)){
            Shape& s=m_doc.shapes[m_sel[0]];
            if (s.type==ShapeType::Text && s.textFrame){
                const QTransform inv=s.transform.inverted();
                const QPointF dl=inv.map(d)-inv.map(m_pressDocPt);   // drag delta in the frame's local coords
                QRectF r=m_textStartRect;
                switch(m_activeHandle){
                  case R: r.setWidth (qMax(20.0, r.width() +dl.x())); break;
                  case L: { double nw=qMax(20.0, r.width() -dl.x()); r.setLeft(r.right()-nw); } break;
                  case B: r.setHeight(qMax(20.0, r.height()+dl.y())); break;
                  case T: { double nh=qMax(20.0, r.height()-dl.y()); r.setTop(r.bottom()-nh); } break;
                  default: break; }
                s.rect=r; reflowText(); emit documentChanged(); emit selectionChanged(); update(); return;
            }
        }
        if (m_moveUndoPending){ pushUndo("Move"); m_moveUndoPending=false; }   // first real move of a body-drag → snapshot now (deferred from press)
        if (m_activeHandle!=HNone){ // resize via bbox scaling
            QRectF b=m_dragStartBounds; QPointF anchor; double sx=1,sy=1;
            QPointF dd=d-m_pressDocPt;
            QRectF nb=b;
            switch(m_activeHandle){
            case TL: nb.setTopLeft(b.topLeft()+dd); break; case T: nb.setTop(b.top()+dd.y()); break;
            case TR: nb.setTopRight(b.topRight()+dd); break; case R: nb.setRight(b.right()+dd.x()); break;
            case BR: nb.setBottomRight(b.bottomRight()+dd); break; case B: nb.setBottom(b.bottom()+dd.y()); break;
            case BL: nb.setBottomLeft(b.bottomLeft()+dd); break; case L: nb.setLeft(b.left()+dd.x()); break; default:break; }
            if((e->modifiers()&Qt::ControlModifier) && (m_activeHandle==TL||m_activeHandle==TR||m_activeHandle==BL||m_activeHandle==BR)){   // Ctrl = lock aspect ratio
                const double ar = b.width()>1e-6 ? b.height()/b.width() : 1.0;
                double nw=qAbs(nb.width()), nh=qAbs(nb.height());
                if(nw*ar>nh) nh=nw*ar; else if(ar>1e-6) nw=nh/ar;
                switch(m_activeHandle){
                  case TL: nb=QRectF(b.right()-nw, b.bottom()-nh, nw, nh); break;
                  case TR: nb=QRectF(b.left(),     b.bottom()-nh, nw, nh); break;
                  case BL: nb=QRectF(b.right()-nw, b.top(),       nw, nh); break;
                  default: nb=QRectF(b.left(),     b.top(),       nw, nh); break; }
            }
            if (qAbs(nb.width())<1) nb.setWidth(1); if(qAbs(nb.height())<1) nb.setHeight(1);
            sx=nb.width()/b.width(); sy=nb.height()/b.height();
            anchor=nb.topLeft(); QPointF banch=b.topLeft();
            const double gs = std::sqrt(qMax(1e-9, std::abs(sx*sy)));   // geometric-mean scale for stroke width
            { auto tg=selTargets(); for(int k=0;k<tg.size()&&k<m_dragStartXf.size();++k){ QTransform t; t.translate(anchor.x(),anchor.y()); t.scale(sx,sy); t.translate(-banch.x(),-banch.y()); tg[k]->transform=m_dragStartXf[k]*t;
                if(m_doc.scaleLineWidths && k<m_dragStartWidth.size()) tg[k]->stroke.width = m_dragStartWidth[k]*gs; } }   // Xara default: line width scales with the object
            emit documentChanged(); update(); return; }
        // move (with snapping to page/object edges & centres; hold Alt to bypass)
        QPointF dd=d-m_pressDocPt;
        if(e->modifiers()&Qt::ShiftModifier){   // constrain to 0/45/90° (8-way), keeping the drag length
            const double ang=std::round(std::atan2(dd.y(),dd.x())/(M_PI/4.0))*(M_PI/4.0), len=std::hypot(dd.x(),dd.y());
            dd=QPointF(std::cos(ang)*len, std::sin(ang)*len); }
        m_guideX.clear(); m_guideY.clear();
        if(!(e->modifiers()&Qt::AltModifier)) dd += snapAdjust(m_dragStartBounds.translated(dd), m_guideX, m_guideY);
        { auto tg=selTargets(); for(int k=0;k<tg.size()&&k<m_dragStartXf.size();++k){ QTransform t=m_dragStartXf[k]; t=t*QTransform::fromTranslate(dd.x(),dd.y()); tg[k]->transform=t; } }
        emit documentChanged(); update(); return;
    }
    case Tool::Node: {
        if(m_nodeShape>=0 && m_bendSub>=0){                       // dragging a segment to bend its curve
            if(!m_bendActive){                                   // drag threshold: a stray sub-3px move is not a bend
                if(QLineF(w, m_pressView).length() < 3.0){ update(); return; }
                m_bendActive=true; pushUndo("Reshape curve");
            }
            const QPointF lp=nodeXf().inverted().map(d);
            reshapePathSegment(m_bendSub, m_bendIdx, m_bendT, m_bendOut0, m_bendIn0, m_bendGrab, lp);
            emit selectionChanged(); update(); return;
        }
        if(m_nodeShape>=0 && m_nodeSub>=0 && m_nodeIndex>=0){
            const QTransform inv=nodeXf().inverted();
            QPointF lp=inv.map(d);
            PathNode& nd=m_nodes[m_nodeSub].nodes[m_nodeIndex];
            if(m_nodePart==NPAnchor){ QPointF dd=lp-nd.anchor;
                if(m_nodeSelSet.size()>1 && m_nodeSelSet.contains(qMakePair(m_nodeSub,m_nodeIndex))){   // move the whole selection
                    for(const auto& pr : m_nodeSelSet){ if(pr.first<m_nodes.size() && pr.second<m_nodes[pr.first].nodes.size()){
                        PathNode& x=m_nodes[pr.first].nodes[pr.second]; x.anchor+=dd; if(x.hasIn)x.in+=dd; if(x.hasOut)x.out+=dd; } }
                } else { nd.anchor+=dd; if(nd.hasIn)nd.in+=dd; if(nd.hasOut)nd.out+=dd; } }
            else if(m_nodePart==NPIn){ nd.in=lp; nd.hasIn=true;
                if(nd.smooth && nd.hasOut){
                    if(nd.symmetric) nd.out=nd.anchor*2.0-lp;                        // mirror: collinear + equal length
                    else { QPointF din=nd.anchor-lp; double dl=std::hypot(din.x(),din.y());  // smooth: collinear, keep out's length
                           double lo=std::hypot(nd.out.x()-nd.anchor.x(), nd.out.y()-nd.anchor.y());
                           if(dl>1e-6) nd.out=nd.anchor+din/dl*lo; } } }
            else { nd.out=lp; nd.hasOut=true;
                if(nd.smooth && nd.hasIn){
                    if(nd.symmetric) nd.in=nd.anchor*2.0-lp;
                    else { QPointF dout=nd.anchor-lp; double dl=std::hypot(dout.x(),dout.y());
                           double li=std::hypot(nd.in.x()-nd.anchor.x(), nd.in.y()-nd.anchor.y());
                           if(dl>1e-6) nd.in=nd.anchor+dout/dl*li; } } }
            writeBackNodes(); update();
        }
        else if(m_rubber){ m_rubberRect=QRectF(m_pressView, w).normalized(); update(); }   // node marquee: live feedback
        return;
    }
    case Tool::Rect: case Tool::Ellipse: case Tool::Polygon: case Tool::Line: {
        if(m_sel.size()==1 && (m_activeHandle==EllipseStart||m_activeHandle==EllipseEnd||m_activeHandle==StarDepth||m_activeHandle==PolyCurv)){
            Shape& s=m_doc.shapes[m_sel[0]];
            const QPointF lp=s.transform.inverted().map(d); const QPointF c=s.rect.center();
            const double rx=qMax(1e-6,s.rect.width()/2.0), ry=qMax(1e-6,s.rect.height()/2.0);
            if(m_activeHandle==EllipseStart||m_activeHandle==EllipseEnd){
                double ang=std::atan2(-(lp.y()-c.y()), lp.x()-c.x())*180.0/M_PI;   // Qt CCW, y-up degrees
                if(m_activeHandle==EllipseStart){ double end=s.ellipseStart+s.ellipseSweep; s.ellipseStart=ang; double sw=end-ang; while(sw<=0)sw+=360; while(sw>360)sw-=360; s.ellipseSweep=sw; }
                else { double sw=ang-s.ellipseStart; while(sw<=0)sw+=360; while(sw>360)sw-=360; s.ellipseSweep=sw; }
            } else if(m_activeHandle==StarDepth){
                s.polyStarDepth=qBound(0.05, std::hypot((lp.x()-c.x())/rx,(lp.y()-c.y())/ry), 0.98);
            } else {   // PolyCurv: project the drag onto the first edge's outward normal (view space, same math as the handle)
                const QPointF Av=toView(s.transform.map(polyVertexLocal(s,0))), Bv=toView(s.transform.map(polyVertexLocal(s,1)));
                const QPointF Cv=toView(s.transform.map(s.rect.center())), midv=(Av+Bv)/2.0;
                QPointF ev=Bv-Av; double Lv=std::hypot(ev.x(),ev.y());
                QPointF nv = Lv>1e-9 ? QPointF(-ev.y()/Lv, ev.x()/Lv) : QPointF(0,-1);
                if(QPointF::dotProduct(nv, midv-Cv)<0) nv=-nv;
                double proj=QPointF::dotProduct(w-midv, nv);
                s.polyCurvature=qBound(-2.0, Lv>1e-6? (proj-16.0)/(Lv*0.3642) : 0.0, 2.0);
            }
            emit documentChanged(); emit selectionChanged(); update(); return;
        }
        update(); return;   // preview drawn in overlay via m_curDocPt (handled in release for commit; live preview below)
    }
    case Tool::Region: {
        if (m_regionFreehand && m_dragging) m_freePts<<d;
        update(); return;
    }
    case Tool::Table: {
        if (m_tableDivShape>=0 && (m_tableColDivider>=0||m_tableRowDivider>=0)){   // drag a row/column divider
            Shape& ts=m_doc.shapes[m_tableDivShape]; const QTransform inv=ts.transform.inverted();
            const QPointF dl=inv.map(d)-inv.map(m_pressDocPt);
            if(m_tableColDivider>=0) ts.table.colW[m_tableColDivider]=qMax(8.0, m_tableDivStart+dl.x());
            else                     ts.table.rowH[m_tableRowDivider]=qMax(8.0, m_tableDivStart+dl.y());
            emit documentChanged(); update(); return;
        }
        if (m_cellRangeDrag && m_tableShape>=0){ int r,c; if(tableCellAt(m_tableShape,d,r,c)) tableSelectCell(m_tableShape,r,c,true); }
        update(); return;
    }
    case Tool::Text: {   // dragging out from the just-created point text → grow it into a wrapping frame
        if(m_textDragPending && editingText() && m_textEdit>=0 && m_textEdit<(int)m_doc.shapes.size()){
            const QRectF r=QRectF(m_textDragStart,d).normalized();
            if(r.width()>24 || r.height()>16){
                Shape& s=m_doc.shapes[m_textEdit];
                s.textFrame=true; s.rect=QRectF(0,0, qMax(24.0,r.width()), qMax(16.0,r.height()));
                s.transform=QTransform::fromTranslate(r.left(), r.top());
                emit documentChanged(); emit selectionChanged(); update();
            }
        }
        return;
    }
    case Tool::Freehand: { m_freePts<<d; update(); return; }
    case Tool::Pen: { update(); return; }
    case Tool::Fill: {
        if(m_dragging && !m_sel.isEmpty()){ Shape&s=m_doc.shapes[m_sel.first()]; QPointF lp=s.transform.inverted().map(d);
            if(m_activeHandle==GradP0) s.fill.p0=lp;
            else if(m_activeHandle==GradP2) s.fill.p2=lp;
            else if(m_activeHandle==GradMid){ QPointF dl=lp - s.transform.inverted().map(m_pressDocPt); s.fill.p0=m_gradStart0+dl; s.fill.p1=m_gradStart1+dl; s.fill.p2=m_gradStart2+dl; }
            else s.fill.p1=lp;
            emit documentChanged(); update(); }
        return;
    }
    case Tool::Transparency: {
        if(m_transpMode==1){                                          // PAINT MASK mode
            if(m_maskStroke){
                if(m_maskShape<0 || m_maskShape>=m_doc.shapes.size() || m_doc.shapes[m_maskShape].transp.geom!=4 || m_doc.shapes[m_maskShape].transp.maskBmp.isNull()){ resetMaskStroke(); update(); return; }
                QPointF imgPt; if(docToMaskPixel(m_maskShape,d,imgPt)){
                    Shape& s=m_doc.shapes[m_maskShape];
                    double dlen=std::hypot(imgPt.x()-m_maskLastImg.x(),imgPt.y()-m_maskLastImg.y());
                    double step=qMax(1.0,m_maskNib*0.25); int n=qMax(1,int(dlen/step));
                    for(int i=1;i<=n;++i){ double t=double(i)/n; QPointF dp(qRound(m_maskLastImg.x()*(1-t)+imgPt.x()*t),qRound(m_maskLastImg.y()*(1-t)+imgPt.y()*t));
                        maskDab(s, dp, m_maskErase?0.0:255.0); }
                    m_maskLastImg=imgPt; emit documentChanged();
                }
            }
            update(); return; }
        if(m_dragging && !m_sel.isEmpty()){ Shape&s=m_doc.shapes[m_sel.first()]; QPointF lp=s.transform.inverted().map(d);
            if(m_activeHandle==GradP0) s.transp.p0=lp;
            else if(m_activeHandle==GradP2) s.transp.p2=lp;
            else if(m_activeHandle==GradMid){ QPointF dl=lp - s.transform.inverted().map(m_pressDocPt); s.transp.p0=m_gradStart0+dl; s.transp.p1=m_gradStart1+dl; s.transp.p2=m_gradStart2+dl; }
            else s.transp.p1=lp;
            emit documentChanged(); emit selectionChanged(); update(); }
        return;
    }
    case Tool::Blend: {
        if(m_activeHandle==BlendStep && m_sel.size()==1){   // drag the step handle → change the live blend's step count
            Shape& g=m_doc.shapes[m_sel[0]];
            if(g.type==ShapeType::Group && g.blendSteps>0){
                int ns=qBound(1, int(m_smartStartParam + (d.x()-m_pressDocPt.x())*m_zoom/12.0 + 0.5), 100);   // ~12 screen-px per step, zoom-independent
                if(ns!=g.blendSteps){ g.blendSteps=ns; emit documentChanged(); emit selectionChanged(); } }
            update(); return;
        }
        update(); return;   // drag-to-blend preview line is drawn in the overlay
    }
    case Tool::Mould: {
        if(m_mouldDrag>=0 && m_mouldDrag<4){ moveMouldCorner(m_mouldDrag,d); update(); }
        else if(m_mouldDrag>=4){ moveMouldCtrl(m_mouldDrag-4,d); update(); }
        return;
    }
    case Tool::Shadow: {
        if(m_activeHandle==ShadowRake && m_sel.size()==1){         // drag the floor-shadow tip: lean (shear) + length (scale)
            if(!m_shadowDidDrag){ if(QLineF(w,m_pressView).length()<3.0){ update(); return; } m_shadowDidDrag=true; pushUndo("Shadow rake"); }   // no undo on a bare click
            Shape& s=m_doc.shapes[m_sel[0]]; const QRectF b=s.docBounds();
            if(b.height()>0){ const double H=b.height();
                const double fsh=qBound(-3.0, (b.center().x()+s.shadow.offset.x() - d.x())/H, 3.0);
                const double fsc=qBound(0.05, (b.bottom() - d.y())/H, 2.0);
                if(qAbs(fsh-s.shadow.floorShear)>1e-6 || qAbs(fsc-s.shadow.floorScale)>1e-6){ s.shadow.floorShear=fsh; s.shadow.floorScale=fsc; emit documentChanged(); emit selectionChanged(); } }
            update(); return; }
        if(m_activeHandle==ShadowBlur && m_sel.size()==1){         // drag the blur handle outward = softer shadow
            Shape& s=m_doc.shapes[m_sel[0]]; const QRectF b=s.docBounds(); const QPointF br=b.bottomRight()+s.shadow.offset;
            const double nb=qBound(0.0, ((d.x()-br.x())+(d.y()-br.y()))/2.0, 64.0);
            if(qAbs(nb-s.shadow.blur)>1e-6){ s.shadow.blur=nb; emit documentChanged(); emit selectionChanged(); }
            update(); return; }
        if(m_activeHandle==ShadowOff && m_sel.size()==1){          // drag the object = set the shadow offset (cast the shadow)
            if(!m_shadowDidDrag){ if(QLineF(w,m_pressView).length()<3.0){ update(); return; }
                m_shadowDidDrag=true; pushUndo("Shadow offset"); m_doc.shapes[m_sel[0]].shadow.on=true; }
            m_doc.shapes[m_sel[0]].shadow.offset=m_shadowStartOffset + (d-m_pressDocPt);
            emit documentChanged(); emit selectionChanged(); update(); return; }
        return;
    }
    case Tool::Contour: case Tool::Bevel: case Tool::Feather: {
        if(m_activeHandle==EffectScalar && m_sel.size()==1){   // drag the corner handle to set the scalar
            Shape& s=m_doc.shapes[m_sel[0]]; const QPointF br=s.docBounds().bottomRight();
            const double nv=((d.x()-br.x())+(d.y()-br.y()))/2.0;
            if(m_tool==Tool::Contour){ const double v=qBound(-100.0,nv,100.0); if(qAbs(v-s.contour.width)>1e-6){ s.contour.on=true; s.contour.width=v; emit documentChanged(); emit selectionChanged(); } }
            else if(m_tool==Tool::Bevel){ const double v=qBound(1.0,nv,40.0); if(qAbs(v-s.bevel.depth)>1e-6){ s.bevel.on=true; s.bevel.depth=v; emit documentChanged(); emit selectionChanged(); } }
            else { const double v=qBound(0.0,nv,200.0); if(qAbs(v-s.feather)>1e-6){ s.feather=v; emit documentChanged(); emit selectionChanged(); } }
            update(); }
        else if(m_activeHandle==BevelLight && m_sel.size()==1){   // orbit the bevel light: direction = azimuth (y-up)
            Shape& s=m_doc.shapes[m_sel[0]]; const QPointF v=d-s.docBounds().center();
            if(std::hypot(v.x(),v.y())>1e-6){ double nl=std::atan2(-v.y(),v.x())*180.0/M_PI; if(nl<0)nl+=360.0;
                if(qAbs(nl-s.bevel.light)>1e-6){ s.bevel.on=true; s.bevel.light=nl; emit documentChanged(); emit selectionChanged(); } }
            update(); }
        return;
    }
    case Tool::Extrude: {
        if(m_activeHandle==ExtrudeVec && m_sel.size()==1){   // drag the vanishing point: distance = depth, direction = angle
            Shape& s=m_doc.shapes[m_sel[0]]; const QPointF ctr=s.docBounds().center(); const QPointF v=d-ctr;
            const double depth=qBound(0.0, std::hypot(v.x(),v.y()), 400.0);
            const double ang=std::atan2(v.y(),v.x())*180.0/M_PI;
            if(qAbs(depth-s.extrude.depth)>1e-6 || qAbs(ang-s.extrude.angle)>1e-6){ s.extrude.on=true; s.extrude.depth=depth; s.extrude.angle=ang; emit documentChanged(); emit selectionChanged(); }
            update(); }
        else if(m_activeHandle==ExtrudeLight && m_sel.size()==1){   // orbit the light: direction = azimuth
            Shape& s=m_doc.shapes[m_sel[0]]; const QPointF v=d-s.docBounds().center();
            if(std::hypot(v.x(),v.y())>1e-6){ int nl=int(std::lround(std::atan2(v.y(),v.x())*180.0/M_PI)); nl=((nl%360)+360)%360;
                if(nl!=s.extrude.light){ s.extrude.on=true; s.extrude.light=nl; emit documentChanged(); emit selectionChanged(); } }
            update(); }
        return;
    }
    case Tool::Eraser: {
        if (m_softEraseShape>=0 && (e->buttons()&Qt::LeftButton)){
            if(m_softEraseShape<m_doc.shapes.size() && m_doc.shapes[m_softEraseShape].fill.type==FillType::Bitmap)
                { softEraseAt(m_softEraseShape,d); emit documentChanged(); }
            else m_softEraseShape=-1;
        }
        update(); return;
    }
    case Tool::Clone: {
        if (m_cloneStroke){
            if (m_cloneShape<0 || m_cloneShape>=m_doc.shapes.size()
                || m_doc.shapes[m_cloneShape].fill.type!=FillType::Bitmap || m_doc.shapes[m_cloneShape].fill.image.isNull()){
                resetCloneStroke(); update(); return;   // the photo went away / changed mid-stroke
            }
            QPointF imgPt; if(docToImagePixel(m_cloneShape,d,imgPt)){
                Shape& s=m_doc.shapes[m_cloneShape];
                double dlen=std::hypot(imgPt.x()-m_cloneLastImg.x(), imgPt.y()-m_cloneLastImg.y());
                double step=qMax(1.0, m_cloneNib*0.25); int n=qMax(1,int(dlen/step));
                for(int i=1;i<=n;++i){ double t=double(i)/n;
                    QPointF dp(qRound(m_cloneLastImg.x()*(1.0-t)+imgPt.x()*t), qRound(m_cloneLastImg.y()*(1.0-t)+imgPt.y()*t));   // integer dab centres (crisp source sampling)
                    if(m_cloneMode==2) redeyeDab(s, dp); else cloneDab(s, m_cloneSnap, dp, dp+m_cloneOffset); }
                m_cloneLastImg=imgPt; emit documentChanged();
            }
        }
        update(); return;
    }
    default: return;
    }
}

void Canvas::mouseReleaseEvent(QMouseEvent* e) {
    const QPointF w=e->position(); const QPointF d=toDoc(w);
    clearDragProxy();   // any live drag is ending; drop the proxy bitmaps → next paint is a full, correct render
    if (m_textDragging){ m_textDragging=false; return; }   // end a text drag-selection
    if (m_indentDrag>=0){ m_indentDrag=-1; update(); return; }   // finish an indent-marker drag
    if (m_guideDrag!=GDNone) {   // finish a guide drag; drop back onto the ruler or off-canvas = delete
        const bool horiz=(m_guideDrag==GDNewH||m_guideDrag==GDMoveH);
        QVector<double>& arr = horiz? m_guideHpos : m_guideVpos;
        const bool del = w.x()<kRuler || w.y()<kRuler || w.x()<0 || w.y()<0 || w.x()>width() || w.y()>height();
        if (del && m_dragGuideIdx>=0 && m_dragGuideIdx<arr.size()) arr.removeAt(m_dragGuideIdx);
        m_guideDrag=GDNone; m_dragGuideIdx=-1; update(); return;
    }
    if (m_cropMode){ m_cropHandle=-1; return; }
    if (m_straightenMode){ m_straightenGrab=-1; return; }
    if (m_panning){ m_panning=false; setCursor(m_tool==Tool::Pan?Qt::OpenHandCursor:Qt::ArrowCursor); return; }
    if (e->button()!=Qt::LeftButton) return;

    switch (m_tool) {
    case Tool::Zoom: {
        m_rubber=false; QRectF r=QRectF(m_pressDocPt,d).normalized();
        if (r.width()>4 && r.height()>4) zoomToRect(r);                    // marquee zoom
        else if (e->modifiers()&Qt::AltModifier) zoomOut(); else zoomIn(); // plain click
        update(); return;
    }
    case Tool::Select: {
        if (m_rubber){ m_rubber=false; QRectF rd=QRectF(m_pressDocPt,d).normalized(); m_sel.clear();
            for(int i=0;i<m_doc.shapes.size();++i) if(rd.intersects(m_doc.shapes[i].docBounds())) m_sel.push_back(i);
            emit selectionChanged(); update(); }
        else if (m_dragging && m_activeHandle==HNone && !m_rotating && !m_sel.isEmpty() && !inGroup()
                 && (e->modifiers()&Qt::ControlModifier) && m_dragStartXf.size()==m_sel.size()
                 && QLineF(selBoundsDoc().center(), m_dragStartBounds.center()).length()>1.0) {
            // Ctrl during a move → drop a copy of the selection at its ORIGINAL position (moved ones stay).
            for (int k=0;k<m_sel.size();++k){ Shape cp=m_doc.shapes[m_sel[k]]; cp.transform=m_dragStartXf[k]; cp.linkId=0; m_doc.addShape(cp); }
            emit documentChanged();
        }
        // Re-clicking the already-selected object without dragging flips the handles resize<->rotate/skew (Xara).
        else if (m_toggleRotCandidate && m_activeHandle==HNone && QLineF(w,m_pressView).length()<3.0){
            m_rotateMode=!m_rotateMode; if(!m_rotateMode) m_rotPivotSet=false; emit selectionChanged();
        }
        m_toggleRotCandidate=false;
        m_dragging=false; m_activeHandle=HNone; m_rotating=false;
        m_guideX.clear(); m_guideY.clear(); update(); break;
    }
    case Tool::Rect: case Tool::Ellipse: case Tool::Polygon: case Tool::Line: {
        if(m_activeHandle==EllipseStart||m_activeHandle==EllipseEnd||m_activeHandle==StarDepth||m_activeHandle==PolyCurv){
            m_activeHandle=HNone; m_dragging=false; update(); break; }   // finished editing a shape param, not drawing a new shape
        if(!m_dragging){ m_activeHandle=HNone; break; }   // a press that only selected an existing shape (not a new-shape drag) commits nothing
        // Ctrl constrains creation: Rect→square, Ellipse→circle, Polygon→regular bounds, Line→15° angle snap.
        const bool cons = e->modifiers()&Qt::ControlModifier;
        const QPointF far = !cons ? d : (m_tool==Tool::Line ? constrainLineAngle(m_pressDocPt,d) : constrainSquareCorner(m_pressDocPt,d));
        QRectF r=QRectF(m_pressDocPt,far).normalized();
        if (r.width()<2 && r.height()<2 && m_tool!=Tool::Line){   // a click, not a drag → don't spawn a zero-size shape
            if(m_tool==Tool::Ellipse || m_tool==Tool::Polygon){   // ...instead select an existing same-type shape under the cursor (so its edit handles appear)
                int hit=hitShape(m_pressDocPt);
                const ShapeType want = (m_tool==Tool::Ellipse)? ShapeType::Ellipse : ShapeType::Polygon;
                if(hit>=0 && m_doc.shapes[hit].type==want && m_sel!=QVector<int>{hit}){ m_sel={hit}; emit selectionChanged(); } }
            m_dragging=false; update(); break; }
        Shape s=styledShape();
        if (m_tool==Tool::Rect){ s.type=ShapeType::Rect; s.rect=QRectF(0,0,r.width(),r.height()); s.transform=QTransform::fromTranslate(r.x(),r.y()); s.name="Rectangle"; }
        else if (m_tool==Tool::Ellipse){ s.type=ShapeType::Ellipse; s.rect=QRectF(0,0,r.width(),r.height()); s.transform=QTransform::fromTranslate(r.x(),r.y()); s.name="Ellipse"; }
        else if (m_tool==Tool::Polygon){ s.type=ShapeType::Polygon; s.polySides=m_polySides; s.polyStar=m_polyStar; s.rect=QRectF(0,0,r.width(),r.height()); s.transform=QTransform::fromTranslate(r.x(),r.y()); s.name=m_polyStar?"Star":"Polygon"; }
        else { s.type=ShapeType::Path; QPainterPath p; p.moveTo(m_pressDocPt); p.lineTo(far); s.path=p; s.fill.type=FillType::None; if(!s.stroke.enabled){s.stroke.enabled=true;} s.name="Line"; }
        commitNewShape(s,"Draw");
        m_dragging=false; break;
    }
    case Tool::Region: {
        m_dragging=false;
        if (m_regionMagnetic){ break; }                 // magnetic lasso is click-anchor + double-click driven
        int tgt = m_sel.isEmpty()? hitShape(d) : m_sel.first();
        if (tgt<0){ m_freePts.clear(); update(); break; }
        QPainterPath docReg;
        if (m_regionFreehand) {
            if (m_freePts.size()<3){ m_freePts.clear(); update(); break; }
            docReg=smoothPath(m_freePts); docReg.closeSubpath(); m_freePts.clear();
        } else {
            QRectF r=QRectF(m_pressDocPt,d).normalized();
            if (r.width()<4 || r.height()<4){ update(); break; }
            if(m_regionEllipse) docReg.addEllipse(r); else docReg.addRect(r);
        }
        QPainterPath localReg = m_doc.shapes[tgt].transform.inverted().map(docReg);   // store in the object's local space
        m_sel={tgt};
        if (m_regionEffect>0) paintEffectRegion(tgt, localReg, m_regionEffect);       // Effect Painter: scoped adjustment overlay
        else setShapeRegion(tgt, localReg);                                           // classic: clip the object to the region
        break;
    }
    case Tool::Table: {
        if (m_tableDivShape>=0){ m_tableDivShape=-1; m_tableColDivider=-1; m_tableRowDivider=-1; m_dragging=false; emit documentChanged(); update(); break; }   // finished a divider drag
        if (m_cellRangeDrag){ m_cellRangeDrag=false; update(); break; }   // finished selecting a cell range
        if (!m_dragging) break;
        QRectF r=QRectF(m_pressDocPt,d).normalized();
        if (r.width()<8 || r.height()<8){ r=QRectF(r.topLeft(), QSizeF(qMax(r.width(),240.0), qMax(r.height(),120.0))); }
        Shape s; s.type=ShapeType::Table; s.name="Table";
        s.table=Table::grid(m_tableRows, m_tableCols, r.width(), r.height());
        s.transform=QTransform::fromTranslate(r.x(),r.y());
        s.fill.type=FillType::None; s.stroke.enabled=false;
        commitNewShape(s,"Table");
        m_dragging=false; break;
    }
    case Tool::Text:   // point text was created (and a drag already grew it to a frame) on press/move; just end the gesture
        m_textDragPending=false; m_dragging=false; break;
    case Tool::Freehand: {
        if (m_freePts.size()>=2 && freehandRedraw(m_freePts)) { m_freePts.clear(); m_dragging=false; break; }   // stroke over the selected path → reshape it
        if (m_freePts.size()>=2){ Shape s=styledShape(); s.type=ShapeType::Path; s.path=smoothPath(m_freePts, m_freeSmooth); s.fill.type=FillType::None; s.name="Freehand"; commitNewShape(s,"Freehand"); }
        m_freePts.clear(); m_dragging=false; break;
    }
    case Tool::Pen: m_penHandleDrag=false; update(); break;   // anchor's handle now fixed
    case Tool::Node: {
        if (m_rubber){ m_rubber=false; selectNodesInRect(QRectF(m_pressDocPt,d).normalized()); }
        m_nodeSub=-1; m_nodeIndex=-1; m_bendSub=-1; m_bendActive=false; m_dragging=false; update(); break;  // keep shape under edit
    }
    case Tool::Mould: m_mouldDrag=-1; break;
    case Tool::Blend: {
        if(m_activeHandle==BlendStep){ m_activeHandle=HNone; m_dragging=false; m_blendFromShape=-1; update(); break; }
        if(m_blendFromShape>=0 && m_blendFromShape<(int)m_doc.shapes.size()){
            int hit=hitShape(d);   // dragged from one object onto a DIFFERENT one → make a live blend between them
            if(hit>=0 && hit!=m_blendFromShape){ m_sel={m_blendFromShape, hit}; makeLiveBlend(6); }
        }
        m_blendFromShape=-1; m_dragging=false; update(); break;
    }
    case Tool::Shadow: {
        if(m_activeHandle==ShadowOff && !m_shadowDidDrag && m_sel.size()==1){   // a click (no drag) toggles the shadow on/off
            pushUndo("Shadow"); Shape& s=m_doc.shapes[m_sel[0]]; s.shadow.on=!s.shadow.on;
            if(s.shadow.on && s.shadow.offset.isNull()) s.shadow.offset=QPointF(6,8);
            emit documentChanged(); emit selectionChanged();
        }
        m_activeHandle=HNone; m_dragging=false; m_shadowDidDrag=false; update(); break;
    }
    case Tool::Contour: case Tool::Bevel: case Tool::Feather: m_dragging=false; m_activeHandle=HNone; break;
    case Tool::Extrude: m_dragging=false; m_activeHandle=HNone; break;
    case Tool::Fill: m_dragging=false; m_activeHandle=HNone; break;
    case Tool::Transparency: if(m_transpMode==1){ if(m_maskStroke){ m_maskStroke=false; emit documentChanged(); emit selectionChanged(); } } else { m_dragging=false; m_activeHandle=HNone; } break;
    case Tool::Eraser: if(m_softEraseShape>=0){ m_softEraseShape=-1; emit documentChanged(); emit selectionChanged(); } break;
    case Tool::Clone: if(m_cloneStroke){ m_cloneStroke=false; m_cloneSnap=QImage(); emit documentChanged(); emit selectionChanged(); } break;
    default: break;
    }
}

void Canvas::mouseDoubleClickEvent(QMouseEvent* e) {
    if (m_cropMode){ applyCropMode(); return; }   // double-click inside the crop rect applies it
    if (m_straightenMode){ applyStraightenMode(); return; }   // double-click commits the straighten
    if (m_tool==Tool::Region && m_regionMagnetic && m_lassoShape>=0 && m_lassoAnchors.size()>=2) {
        QPainterPath lp=magneticLassoRegion(m_lassoShape, m_lassoAnchors);
        if(!lp.isEmpty()){ setShapeRegion(m_lassoShape, lp); emit statusMessage("Magnetic lasso applied — the photo is clipped to the edge-snapped region."); }
        else emit statusMessage("Magnetic lasso: could not snap a region (try more anchors on the edge).");
        m_lassoAnchors.clear(); m_lassoShape=-1; update();
        return;
    }
    if (m_tool==Tool::Node && m_nodeShape>=0) {
        const QTransform xf=nodeXf();
        // Double-click ON a node → toggle it smooth (collinear handles) ⇄ cusp.
        double best=10; int bs=-1,bn=-1;
        for(int si=0;si<m_nodes.size();++si) for(int ni=0;ni<m_nodes[si].nodes.size();++ni){
            double dist=QLineF(e->position(), toView(xf.map(m_nodes[si].nodes[ni].anchor))).length();
            if(dist<best){best=dist; bs=si; bn=ni;} }
        if(bs>=0){ pushUndo("Node type"); PathNode& nd=m_nodes[bs].nodes[bn];
            int cur = nd.symmetric?2 : nd.smooth?1 : 0;    // double-click cycles cusp → smooth → symmetric → cusp
            setNodeContinuity(nd, (cur+1)%3);
            writeBackNodes(); update(); return; }
        // Double-click ON a segment (no node nearby) → insert a node there.
        const QPointF w=e->position(); double bestSeg=8.0; int ss=-1,si0=-1; double st=0;
        for(int si=0;si<m_nodes.size();++si){ const SubPath& sp=m_nodes[si]; int nn=sp.nodes.size(); int segs=sp.closed?nn:nn-1;
            for(int i=0;i<segs;++i){ int jj=(i+1)%nn; const PathNode& A=sp.nodes[i]; const PathNode& B=sp.nodes[jj];
                QPointF p0=A.anchor,p1=A.hasOut?A.out:A.anchor,p2=B.hasIn?B.in:B.anchor,p3=B.anchor;
                for(int k=0;k<=16;++k){ double t=k/16.0,u=1-t; QPointF pt=p0*u*u*u+p1*3*u*u*t+p2*3*u*t*t+p3*t*t*t;
                    double dist=QLineF(w, toView(xf.map(pt))).length(); if(dist<bestSeg){ bestSeg=dist; ss=si; si0=i; st=t; } } } }
        if(ss>=0){ insertNodeOnSegment(ss,si0,st); emit statusMessage("Node inserted — drag it, or drag the curve to reshape."); update(); }
        return;
    }
    if (m_tool==Tool::Pen && m_penActive) {
        // Double-click finishes an open path (last stray anchor from the dbl-click is dropped).
        if (m_penAnchors.size()>2) m_penAnchors.removeLast(), m_penHandles.removeLast();
        commitPen(false); return;
    }
    if (m_tool==Tool::Fill) {   // double-click a Mesh corner swatch to recolour it
        int corner=hitMeshCorner(e->position());
        if (corner>=0 && m_sel.size()==1){ m_meshCorner=corner; update();
            const Fill& f=m_doc.shapes[m_sel.first()].fill;
            QColor cur = corner<f.meshColors.size()? f.meshColors[corner] : (f.meshColors.isEmpty()?QColor(Qt::black):f.meshColors.first());
            QColor nc = ColorEditor::pick(this, cur, this);
            if (nc.isValid()) setMeshCornerColor(corner, nc);
        }
        return;
    }
    if (m_tool==Tool::Shadow && m_sel.size()==1) {   // double-click the shadow colour swatch to recolour the shadow
        QPointF cs; if (shadowColorSwatch(cs) && QLineF(e->position(),cs).length()<9){
            QColor nc = ColorEditor::pick(this, m_doc.shapes[m_sel[0]].shadow.color, this);
            if (nc.isValid()){ pushUndo("Shadow colour"); m_doc.shapes[m_sel[0]].shadow.color=nc; emit documentChanged(); emit selectionChanged(); update(); }
        }
        return;
    }
    if (m_tool==Tool::Contour && m_sel.size()==1) {  // double-click the contour colour swatch(es) to recolour the contour
        QPointF cs;
        if (contourColorSwatch(cs) && QLineF(e->position(),cs).length()<9){
            QColor nc = ColorEditor::pick(this, m_doc.shapes[m_sel[0]].contour.color, this);
            if (nc.isValid()){ pushUndo("Contour colour"); m_doc.shapes[m_sel[0]].contour.color=nc; emit documentChanged(); emit selectionChanged(); update(); }
        } else if (contourColor2Swatch(cs) && QLineF(e->position(),cs).length()<9){   // OUTER graduation colour
            const Shape& s0=m_doc.shapes[m_sel[0]];
            QColor cur = s0.contour.color2.isValid() ? s0.contour.color2 : s0.contour.color;
            QColor nc = ColorEditor::pick(this, cur, this);
            if (nc.isValid()){ pushUndo("Contour outer colour"); m_doc.shapes[m_sel[0]].contour.color2=nc; emit documentChanged(); emit selectionChanged(); update(); }
        }
        return;
    }
    if (m_tool==Tool::Bevel && m_sel.size()==1) {    // double-click the bevel light-colour swatch to recolour the highlight
        QPointF cs; if (bevelColorSwatch(cs) && QLineF(e->position(),cs).length()<9){
            QColor nc = ColorEditor::pick(this, m_doc.shapes[m_sel[0]].bevel.lightColor, this);
            if (nc.isValid()){ pushUndo("Bevel colour"); m_doc.shapes[m_sel[0]].bevel.lightColor=QColor(nc.red(),nc.green(),nc.blue()); emit documentChanged(); emit selectionChanged(); update(); }
        }
        return;
    }
    if (m_tool==Tool::Extrude && m_sel.size()==1) {  // double-click the extrude wall-colour swatch to recolour the 3-D sides/back
        QPointF cs; if (extrudeColorSwatch(cs) && QLineF(e->position(),cs).length()<9){
            const Shape& s=m_doc.shapes[m_sel[0]];
            QColor cur = s.extrude.color.isValid() ? s.extrude.color : s.fill.representativeColor();
            QColor nc = ColorEditor::pick(this, cur, this);
            if (nc.isValid()){ pushUndo("Extrude colour"); m_doc.shapes[m_sel[0]].extrude.color=QColor(nc.red(),nc.green(),nc.blue()); emit documentChanged(); emit selectionChanged(); update(); }
        }
        return;
    }
    if (m_tool==Tool::Table){                       // double-click a cell → edit its text
        const QPointF d=toDoc(e->position()); int hit=hitShape(d);
        if(hit>=0 && m_doc.shapes[hit].type==ShapeType::Table){ int r,c;
            if(tableCellAt(hit,d,r,c)){ tableSelectCell(hit,r,c,false);
                Table t=m_doc.shapes[hit].table; int ar=r,ac=c; tableAnchorOf(t,ar,ac);
                bool ok=false; QString cur=m_doc.shapes[hit].table.at(ar,ac).text;
                QString v=QInputDialog::getMultiLineText(this,"Cell","Cell text:",cur,&ok);
                if(ok) tableSetCellText(r,c,v); } }
        return;
    }
    if (m_tool==Tool::Select){ const QPointF d=toDoc(e->position());
        // Already inside a group: double-clicking a child group descends another level (select-into-group).
        if (inGroup()) {
            Shape* c=enteredContainer();
            if (c) { const int ci=hitChildInGroup(*c, containerXf(), d);
                if (ci>=0 && c->children[ci].type==ShapeType::Group) {
                    m_enterPath.push_back(ci); m_subSel.clear();
                    Shape* c2=enteredContainer(); if(c2){ int cj=hitChildInGroup(*c2, containerXf(), d); if(cj>=0) m_subSel={cj}; }
                    m_rotateMode=false; m_rotPivotSet=false; emit selectionChanged(); update();
                } else if (ci>=0) { m_subSel={ci}; emit selectionChanged(); update(); }
            }
            return;
        }
        int hit=hitShape(d);
        if (hit<0) return;
        // Double-clicking an object auto-activates the tool best suited to editing it (Xara-style): text → the
        // Text tool (with a caret / editor), a path → the Shape-editor (Node) tool, an ellipse/polygon → its
        // own parametric tool, a table → the Table tool. A Rectangle and a SmartShape are already fully
        // editable in the Selector (their corner-radius / parametric handles are drawn there), so they stay
        // put; a group is descended into rather than switched.
        Shape& sh=m_doc.shapes[hit];
        // A double-click is an *edit* intent, not a drag or a rotate toggle: neutralise every residual gesture
        // bit left by the two preceding clicks so nothing leaks into the tool we switch to (or, for the
        // stay-in-Select types, into a stray button-held move after this handler). Clearing m_dragStartXf makes
        // any such move a no-op (empty target list); clearing m_rotateMode undoes the first click's reclick
        // resize<->rotate toggle so a double-clicked object never silently lands in rotate/skew mode.
        m_dragging=false; m_activeHandle=HNone; m_moveUndoPending=false; m_dragStartXf.clear();
        m_rotateMode=false; m_rotPivotSet=false; m_toggleRotCandidate=false;
        auto selectHit=[&]{ if(!(m_sel.size()==1 && m_sel[0]==hit)) select({hit}); };
        switch (sh.type){
        case ShapeType::Text:
            selectHit(); setTool(Tool::Text);
            if (Canvas::canInlineEdit(sh)){                           // point text → in-canvas caret at the click
                m_textEditIsNew=false;
                beginTextEdit(hit, sh.textIndexAtLocal(sh.transform.inverted().map(d))); }
            else {                                                    // frame/curve/tabbed/rich → modal editor (multi-line, keeps paragraphs)
                bool ok=false; QString t=QInputDialog::getMultiLineText(this,"Edit Text","Text:",sh.text,&ok);
                if(ok){ pushUndo("Edit text"); Shape& s2=m_doc.shapes[hit]; s2.text=t; s2.textRuns.clear();   // a bulk replace can't keep per-char runs (they'd mis-index) → flatten to the base style
                    emit documentChanged(); update(); } }
            break;
        case ShapeType::Path:    selectHit(); setTool(Tool::Node);    break;   // Shape editor: setTool auto-begins node editing
        case ShapeType::Ellipse:                                              // only a swept pie/arc/segment has editable rim handles in the Ellipse tool;
            if (sh.ellipseMode!=0){ selectHit(); setTool(Tool::Ellipse); }    // a plain full ellipse is fully editable in the Selector, so leave it there
            break;
        case ShapeType::Polygon: selectHit(); setTool(Tool::Polygon); break;  // sides / star-depth / edge-curvature handles
        case ShapeType::Table:   selectHit(); setTool(Tool::Table);           // land on the clicked cell so it's ready to edit (mirrors text dropping a caret)
            { int rr,ccl; if(tableCellAt(hit,d,rr,ccl)) tableSelectCell(hit,rr,ccl,false); } break;
        case ShapeType::Group:                                                // enter the group, sub-selecting the child under the cursor
            enterGroup(hit, hitChildInGroup(sh, sh.transform, d)); break;
        case ShapeType::Rect:    // corner-radius + resize/rotate handles live in the Selector — nothing to switch to
        case ShapeType::Smart:   // SmartShape parametric handles live in the Selector — stay put
        default: break;
        }
    }
}

void Canvas::wheelEvent(QWheelEvent* e) {
    if(m_radialActive || (m_radialTimer && m_radialTimer->isActive())) radialDisarm();   // Ctrl+wheel is zoom, not the pie
    if (e->modifiers()&Qt::ControlModifier || true) {
        noteZoomPanInteraction();          // fast draft frame during the wheel gesture; crisp repaint once it settles
        double f = e->angleDelta().y()>0 ? 1.15 : 1.0/1.15;
        QPointF before=toDoc(e->position());
        m_zoom=qBound(0.05,m_zoom*f,40.0);
        QPointF after=toView(before);
        m_origin += e->position()-after;
        emit zoomChanged(m_zoom); update();
    }
}

void Canvas::keyPressEvent(QKeyEvent* e) {
    // Ctrl radial menu: bare Ctrl arms the press-and-hold; any other key while armed/open dismisses it
    // (Escape is consumed, everything else falls through so the real shortcut/key still fires).
    // Ctrl+Alt (both held) arms the radial pie. Handle the two modifier keys here so a lone Ctrl or lone Alt —
    // or their autorepeat — neither arms the pie nor trips the "any other key disarms" net below.
    if(e->key()==Qt::Key_Control || e->key()==Qt::Key_Alt){
        if(!e->isAutoRepeat()){
            const auto mods=e->modifiers();   // event snapshot: reliably carries the already-held modifier
            const bool both=((mods&Qt::ControlModifier)||e->key()==Qt::Key_Control) && ((mods&Qt::AltModifier)||e->key()==Qt::Key_Alt);
            // ...but NOT AltGr: on some (EU) layouts AltGr reports as Ctrl+Alt; it carries GroupSwitchModifier
            // (and on Linux, the app's target, AltGr is GroupSwitch, not Ctrl+Alt) → never mistake it for the pie.
            if(both && !(mods&Qt::GroupSwitchModifier)){ radialArm(); return; }
        }
        QWidget::keyPressEvent(e); return;   // lone Ctrl/Alt, autorepeat, or AltGr → normal handling, pie untouched
    }
    if(m_radialActive || (m_radialTimer && m_radialTimer->isActive())){
        const bool esc=(e->key()==Qt::Key_Escape); radialDisarm(); if(esc) return;
    }
    if (m_cropMode){ if(e->key()==Qt::Key_Return||e->key()==Qt::Key_Enter) applyCropMode();
                     else if(e->key()==Qt::Key_Escape) cancelCropMode(); return; }
    if (m_straightenMode){ if(e->key()==Qt::Key_Return||e->key()==Qt::Key_Enter) applyStraightenMode();
                     else if(e->key()==Qt::Key_Escape) cancelStraightenMode(); return; }
    // In-canvas text editing: type, navigate, select and edit directly on the canvas.
    if (editingText()){
        const bool sh=e->modifiers()&Qt::ShiftModifier;
        switch(e->key()){
        case Qt::Key_Escape:    endTextEdit(); return;
        case Qt::Key_Return: case Qt::Key_Enter: textInsert("\n"); return;
        case Qt::Key_Backspace: textBackspace(); return;
        case Qt::Key_Delete:    textDelete(); return;
        case Qt::Key_Left:  textMoveCaret(-1,sh); return;
        case Qt::Key_Right: textMoveCaret(+1,sh); return;
        case Qt::Key_Up:    textMoveCaret(-3,sh); return;
        case Qt::Key_Down:  textMoveCaret(+3,sh); return;
        case Qt::Key_Home:  textMoveCaret(-2,sh); return;
        case Qt::Key_End:   textMoveCaret(+2,sh); return;
        default: break;
        }
        if(e->matches(QKeySequence::SelectAll)){ textSelectAll(); return; }
        if(e->matches(QKeySequence::Copy) || e->matches(QKeySequence::Cut)){
            if(textHasSelection()){ const QString& tx=m_doc.shapes[m_textEdit].text; int a=qMin(m_caret,m_caretAnchor),b=qMax(m_caret,m_caretAnchor);
                QApplication::clipboard()->setText(tx.mid(a,b-a)); if(e->matches(QKeySequence::Cut)) textBackspace(); } return; }
        if(e->matches(QKeySequence::Paste)){ QString ct=QApplication::clipboard()->text(); if(!ct.isEmpty()) textInsert(ct); return; }
        const QString txt=e->text();
        if(!txt.isEmpty() && txt[0].isPrint() && !(e->modifiers()&(Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier))){ textInsert(txt); return; }
        // Unhandled (e.g. Ctrl+Z undo shortcut) falls through to the normal handler.
    }
    // Table cell keyboard model: Tab/arrows navigate, Return/F2 edits, Delete clears contents,
    // Backspace deletes a fully-selected row/column (else clears contents).
    if (m_tool==Tool::Table && m_tableShape>=0 && m_cellBR>=0){
        const Table& t=m_doc.shapes[m_tableShape].table; QRect sel=tableSelRange();
        switch(e->key()){
        case Qt::Key_Tab:       tableNavigate(0,+1); return;
        case Qt::Key_Backtab:   tableNavigate(0,-1); return;
        case Qt::Key_Right:     tableNavigate(0,+1); return;
        case Qt::Key_Left:      tableNavigate(0,-1); return;
        case Qt::Key_Up:        tableNavigate(-1,0); return;
        case Qt::Key_Down:      tableNavigate(+1,0); return;
        case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_F2: {
            int r=m_cellBR,c=m_cellBC,ar=r,ac=c; tableAnchorOf(t,ar,ac);
            bool ok=false; QString v=QInputDialog::getMultiLineText(this,"Cell","Cell text:",t.at(ar,ac).text,&ok);
            if(ok) tableSetCellText(r,c,v); return; }
        case Qt::Key_Delete: tableClearCells(); return;
        case Qt::Key_Backspace:
            if(sel.height()==t.rows() && sel.width()>0) { for(int i=0;i<sel.width();++i) tableDeleteCol(sel.left()); }
            else if(sel.width()==t.cols() && sel.height()>0) { for(int i=0;i<sel.height();++i) tableDeleteRow(sel.top()); }
            else tableClearCells();
            return;
        case Qt::Key_Escape: clearTableSel(); emit selectionChanged(); update(); return;
        default: break;
        }
    }
    // Node tool: Delete removes the selected node(s) (not the whole shape).
    if (m_tool==Tool::Node && m_nodeShape>=0 && (e->key()==Qt::Key_Delete || e->key()==Qt::Key_Backspace)) {
        if (m_nodeSelSet.size()>1){ deleteSelectedNodes(); return; }
        if (m_nodeSub>=0 && m_nodeIndex>=0){ deleteActiveNode(); return; }
    }
    // Inside a group, Esc pops back out one level (rather than deselecting everything).
    // inGroup() can only be true in the Select tool (any tool switch resets the descent), so no tool guard is needed.
    if (inGroup() && e->key()==Qt::Key_Escape) { exitGroup(); return; }
    switch (e->key()) {
    case Qt::Key_Delete: case Qt::Key_Backspace: deleteSelection(); break;
    case Qt::Key_Escape: m_penActive=false; m_penHandleDrag=false; m_penResume=-1; m_penResumeBase=QPainterPath(); m_penAnchors.clear(); m_penHandles.clear(); selectNone(); setTool(Tool::Select); update(); break;
    case Qt::Key_Return: if(m_tool==Tool::Pen && m_penActive){ commitPen(false); } break;
    case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down: {
        if(e->modifiers()&(Qt::ControlModifier|Qt::AltModifier)) break;   // only bare / Shift+arrow nudges; Ctrl/Alt+arrow belongs to the WM / the pie
        if(m_sel.isEmpty())break; double s=(e->modifiers()&Qt::ShiftModifier)?10:1; QPointF dd(0,0);
        if(e->key()==Qt::Key_Left)dd.setX(-s); if(e->key()==Qt::Key_Right)dd.setX(s); if(e->key()==Qt::Key_Up)dd.setY(-s); if(e->key()==Qt::Key_Down)dd.setY(s);
        applyToSelection([&](Shape& sh){ sh.transform=sh.transform*QTransform::fromTranslate(dd.x(),dd.y()); },"Nudge"); break; }
    default: QWidget::keyPressEvent(e);
    }
}

void Canvas::keyReleaseEvent(QKeyEvent* e) {
    if(e->key()==Qt::Key_Control || e->key()==Qt::Key_Alt){
        // ⚠️ Do NOT trust e->isAutoRepeat(): some X11 setups deliver a HELD key's autorepeat as release+press pairs
        // with isAutoRepeat()==false, so an autorepeat release would spuriously stop the pending pie timer / commit
        // the pie every ~30ms. Trust the LIVE physical key state: if the released modifier is still down, it is a
        // repeat-release — ignore it and let the arm/pie stand.
        const auto live = QApplication::queryKeyboardModifiers();
        const bool stillDown = (e->key()==Qt::Key_Control && (live&Qt::ControlModifier))
                            || (e->key()==Qt::Key_Alt     && (live&Qt::AltModifier));
        if(!stillDown){
            if(m_radialActive){ radialHide(true); return; }                                   // real release → commit the aimed wedge
            if(m_radialTimer && m_radialTimer->isActive()){ m_radialTimer->stop(); return; }   // a quick Ctrl+Alt tap → never bloomed
        }
    }
    QWidget::keyReleaseEvent(e);
}

// Right-click context menu — object/selection ops (single object at top, then multi-object,
// group, arrange, lock) or, on empty pasteboard, Select-All / Paste / Unlock-all.
void Canvas::contextMenuEvent(QContextMenuEvent* e) {
    // Interactive spell-check: right-click a squiggled word while editing → suggestions + Add to Dictionary (G-D).
    if (editingText() && m_spellLive && Speller::instance().ready()) {
        const Shape& s=m_doc.shapes[m_textEdit];
        const QPointF local=s.transform.inverted().map(toDoc(e->pos()));
        const int ci=s.textIndexAtLocal(local);
        for (const auto& r: misspelledRanges(s.text)) {
            if (ci<r.first || ci>r.second) continue;
            const int a=r.first, len=r.second-r.first; const QString word=s.text.mid(a,len);
            QMenu menu(this);
            const QStringList sug=Speller::instance().suggest(word);
            if (sug.isEmpty()) { QAction* na=menu.addAction("(no suggestions)"); na->setEnabled(false); }
            else for (const QString& sg: sug) menu.addAction(sg, this, [this,a,len,sg]{
                pushUndo("Correct spelling"); m_doc.shapes[m_textEdit].text.replace(a,len,sg);
                m_caret=a+sg.size(); m_caretAnchor=-1; emit documentChanged(); emit selectionChanged(); update(); });
            menu.addSeparator();
            menu.addAction("Add to Dictionary", this, [this,word]{ Speller::instance().addUserWord(word); update(); });
            menu.exec(e->globalPos());
            return;
        }
    }
    if (m_tool==Tool::Text || m_penActive) { QWidget::contextMenuEvent(e); return; }
    const QPointF d = toDoc(e->pos());
    int hit = hitShape(d);
    if (hit>=0 && !m_sel.contains(hit)) { m_sel={hit}; emit selectionChanged(); update(); }
    const bool hasSel = !m_sel.isEmpty();
    QMenu menu(this);
    if (hasSel) {
        const Shape& s0 = m_doc.shapes[m_sel.first()];
        if (m_sel.size()==1 && s0.type==ShapeType::Text)
            menu.addAction("Edit text…", this, [this]{ int i=m_sel.first(); bool ok=false;
                QString t=QInputDialog::getText(this,"Edit Text","Text:",QLineEdit::Normal,m_doc.shapes[i].text,&ok);
                if(ok){ pushUndo("Edit text"); m_doc.shapes[i].text=t; emit documentChanged(); update(); } });
        menu.addAction("Cut", this, [this]{ cutSelection(); });
        menu.addAction("Copy", this, [this]{ copySelection(); });
    }
    { QAction* pa=menu.addAction("Paste", this, [this]{ pasteClipboard(); }); pa->setEnabled(!m_clip.isEmpty()); }
    if (hasSel) {
        menu.addAction("Duplicate", this, [this]{ duplicateSelection(); });
        menu.addAction("Delete", this, [this]{ deleteSelection(); });
        menu.addSeparator();
        QMenu* arr=menu.addMenu("Arrange");
        arr->addAction("Bring to Front", this, [this]{ toFront(); });
        arr->addAction("Bring Forwards", this, [this]{ raiseSelection(); });
        arr->addAction("Send Backwards", this, [this]{ lowerSelection(); });
        arr->addAction("Send to Back", this, [this]{ toBack(); });
        if (m_sel.size()>=2 || [&]{ for(int i:m_sel) if(m_doc.shapes[i].type==ShapeType::Group) return true; return false; }()) {
            menu.addSeparator();
            if (m_sel.size()>=2) menu.addAction("Group", this, [this]{ groupSelection(); });
            bool anyGroup=false; for(int i:m_sel) if(m_doc.shapes[i].type==ShapeType::Group) anyGroup=true;
            if (anyGroup) menu.addAction("Ungroup", this, [this]{ ungroupSelection(); });
            if (m_sel.size()>=2) menu.addAction("Soft Group", this, [this]{ makeSoftGroup(); });
        }
        menu.addSeparator();
        menu.addAction("Lock", this, [this]{ lockSelection(); });
    } else {
        menu.addAction("Select All", this, [this]{ selectAll(); });
    }
    // Always offer Unlock-all (locked objects can't be clicked to select).
    { bool anyLocked=false; for(const Shape& s:m_doc.shapes) if(s.locked){ anyLocked=true; break; }
      if(anyLocked){ menu.addSeparator(); menu.addAction("Unlock All", this, [this]{ pushUndo("Unlock all");
          for(Shape& s:m_doc.shapes) s.locked=false; emit documentChanged(); update(); }); } }
    menu.exec(e->globalPos());
}
