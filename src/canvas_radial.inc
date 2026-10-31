// canvas_radial.inc — Ctrl+Alt "pie"/marking menu, #include'd at the bottom of canvas.cpp.
// Hold Ctrl+Alt over the canvas → a two-ring radial menu blooms at the cursor: the INNER ring is the tool
// palette (fling a wedge to switch tool), the OUTER ring is the *active* tool's quick actions/options.
// Selection is marking-menu style: move toward a wedge to highlight it, release Ctrl or Alt to commit. It
// never draws in headless/export renders (m_radialActive is only ever set from live input), so the
// byte-identical corpus is untouched.

namespace {
    constexpr double RM_HOLE      = 22.0;   // dead-zone: release here = cancel
    constexpr double RM_TOOLS_OUT = 66.0;   // inner (tool) ring outer edge
    constexpr double RM_ACT_OUT   = 116.0;  // outer (action) ring outer edge
    constexpr int    RM_HOLD_MS   = 200;    // press-and-hold before the pie appears (so Ctrl+key combos don't flash it)
    // A wedge as a filled ring-sector path (screen angles in degrees, y-down).
    QPainterPath rmWedge(QPointF c, double rIn, double rOut, double a0, double a1){
        QPainterPath p; const int K=10;
        for(int k=0;k<=K;++k){ double a=(a0+(a1-a0)*k/K)*M_PI/180.0; QPointF q=c+QPointF(std::cos(a)*rOut,std::sin(a)*rOut); if(k==0)p.moveTo(q); else p.lineTo(q); }
        for(int k=0;k<=K;++k){ double a=(a1+(a0-a1)*k/K)*M_PI/180.0; p.lineTo(c+QPointF(std::cos(a)*rIn,std::sin(a)*rIn)); }
        p.closeSubpath(); return p;
    }
    // Purpose-drawn monochrome glyphs for the outer-ring ("sub-button") actions, in a ~2r box centred at c.
    void rmGlyph(QPainter& g, const QString& name, QPointF c, double r, const QColor& col){
        g.save();
        QPen pen(col,1.7); pen.setJoinStyle(Qt::RoundJoin); pen.setCapStyle(Qt::RoundCap); g.setPen(pen); g.setBrush(Qt::NoBrush);
        auto P=[&](double x,double y){ return c+QPointF(x*r,y*r); };
        auto ngon=[&](int n,double rad,double rot){ QPainterPath p; for(int i=0;i<n;++i){ double a=rot+i*2*M_PI/n; QPointF v=c+QPointF(std::cos(a)*rad*r,std::sin(a)*rad*r); if(i==0)p.moveTo(v); else p.lineTo(v);} p.closeSubpath(); return p; };
        auto star=[&](int n,double ro,double ri){ QPainterPath p; for(int i=0;i<2*n;++i){ double a=-M_PI/2+i*M_PI/n; double rad=(i&1)?ri:ro; QPointF v=c+QPointF(std::cos(a)*rad*r,std::sin(a)*rad*r); if(i==0)p.moveTo(v); else p.lineTo(v);} p.closeSubpath(); return p; };
        auto badge=[&](bool plus){ QPointF b=P(0.58,0.6); double s=0.3*r; g.drawLine(b+QPointF(-s,0),b+QPointF(s,0)); if(plus) g.drawLine(b+QPointF(0,-s),b+QPointF(0,s)); };
        auto letter=[&](const QString& ch,bool bold,bool ital,double px){ QFont f=g.font(); f.setPixelSize(int(px)); f.setBold(bold); f.setItalic(ital); g.setFont(f); g.setPen(col); g.drawText(QRectF(c.x()-r*1.3,c.y()-r*1.3,r*2.6,r*2.6),Qt::AlignCenter,ch); };
        if(name=="group"){ pen.setStyle(Qt::DashLine); g.setPen(pen); g.drawRoundedRect(QRectF(P(-0.8,-0.8),P(0.8,0.8)),3,3); }
        else if(name=="ungroup"){ g.drawRect(QRectF(P(-0.85,-0.85),P(-0.1,-0.1))); g.drawRect(QRectF(P(0.1,0.1),P(0.85,0.85))); }
        else if(name=="dup"){ g.drawRoundedRect(QRectF(P(-0.8,-0.8),P(0.35,0.35)),2.5,2.5); g.drawRoundedRect(QRectF(P(-0.35,-0.35),P(0.8,0.8)),2.5,2.5); }
        else if(name=="delete"){ g.drawLine(P(-0.75,-0.5),P(0.75,-0.5)); g.drawLine(P(-0.28,-0.5),P(-0.28,-0.75)); g.drawLine(P(0.28,-0.5),P(0.28,-0.75)); g.drawLine(P(-0.28,-0.75),P(0.28,-0.75));
                                 QPainterPath b; b.moveTo(P(-0.55,-0.5)); b.lineTo(P(-0.42,0.78)); b.lineTo(P(0.42,0.78)); b.lineTo(P(0.55,-0.5)); g.drawPath(b);
                                 g.drawLine(P(-0.15,-0.25),P(-0.15,0.55)); g.drawLine(P(0.15,-0.25),P(0.15,0.55)); }
        else if(name=="front"){ g.drawLine(P(-0.8,-0.72),P(0.8,-0.72)); g.drawLine(P(0,0.78),P(0,-0.38)); g.drawLine(P(0,-0.52),P(-0.32,-0.15)); g.drawLine(P(0,-0.52),P(0.32,-0.15)); }
        else if(name=="back"){ g.drawLine(P(-0.8,0.72),P(0.8,0.72)); g.drawLine(P(0,-0.78),P(0,0.38)); g.drawLine(P(0,0.52),P(-0.32,0.15)); g.drawLine(P(0,0.52),P(0.32,0.15)); }
        else if(name.startsWith("rect")){ double rad = name=="rect0"?0.0 : name=="rect1"?0.18*r : name=="rect2"?0.4*r : 0.72*r; g.drawRoundedRect(QRectF(P(-0.75,-0.62),P(0.75,0.62)),rad,rad); }
        else if(name=="ellWhole"){ g.drawEllipse(QRectF(P(-0.8,-0.66),P(0.8,0.66))); }
        else if(name=="ellPie"){ QRectF bx(P(-0.78,-0.78),P(0.78,0.78)); QPainterPath p; p.moveTo(c); p.arcTo(bx,65,290); p.closeSubpath(); g.drawPath(p); }
        else if(name=="ellArc"){ QRectF bx(P(-0.78,-0.78),P(0.78,0.78)); QPainterPath p; p.arcMoveTo(bx,25); p.arcTo(bx,25,290); g.drawPath(p); }
        else if(name=="ellSeg"){ QRectF bx(P(-0.78,-0.78),P(0.78,0.78)); QPainterPath p; p.arcMoveTo(bx,-35); p.arcTo(bx,-35,250); p.closeSubpath(); g.drawPath(p); }
        else if(name=="polyStraight"){ g.drawPath(ngon(5,0.82,-M_PI/2)); }
        else if(name=="polyStar"){ g.drawPath(star(5,0.85,0.37)); }
        else if(name=="polyMinus"){ g.drawPath(ngon(5,0.7,-M_PI/2)); badge(false); }
        else if(name=="polyPlus"){ g.drawPath(ngon(5,0.7,-M_PI/2)); badge(true); }
        else if(name=="bold"){ letter("B",true,false,r*1.95); }
        else if(name=="italic"){ letter("I",false,true,r*1.95); }
        else if(name=="bigger"){ letter("A",true,false,r*1.95); QPointF b=P(0.62,-0.42); g.setPen(pen); g.drawLine(b+QPointF(0,0.42*r),b+QPointF(0,-0.42*r)); g.drawLine(b+QPointF(0,-0.42*r),b+QPointF(-0.24*r,-0.12*r)); g.drawLine(b+QPointF(0,-0.42*r),b+QPointF(0.24*r,-0.12*r)); }
        else if(name=="smaller"){ letter("A",true,false,r*1.35); QPointF b=P(0.62,-0.3); g.setPen(pen); g.drawLine(b+QPointF(0,-0.42*r),b+QPointF(0,0.42*r)); g.drawLine(b+QPointF(0,0.42*r),b+QPointF(-0.24*r,0.12*r)); g.drawLine(b+QPointF(0,0.42*r),b+QPointF(0.24*r,0.12*r)); }
        else if(name=="fillFlat"){ g.setBrush(col); g.setPen(Qt::NoPen); g.drawEllipse(QRectF(P(-0.75,-0.75),P(0.75,0.75))); }
        else if(name=="fillLinear"){ QRectF bx(P(-0.78,-0.6),P(0.78,0.6)); QLinearGradient lg(bx.topLeft(),bx.topRight()); lg.setColorAt(0,col); lg.setColorAt(1,QColor(col.red(),col.green(),col.blue(),36)); g.setBrush(lg); g.setPen(QPen(col,1.2)); g.drawRoundedRect(bx,2,2); }
        else if(name=="fillRadial"){ QRectF bx(P(-0.75,-0.75),P(0.75,0.75)); QRadialGradient rg(c,0.75*r); rg.setColorAt(0,col); rg.setColorAt(1,QColor(col.red(),col.green(),col.blue(),28)); g.setBrush(rg); g.setPen(QPen(col,1.2)); g.drawEllipse(bx); }
        else if(name=="fillNone"){ g.drawEllipse(QRectF(P(-0.72,-0.72),P(0.72,0.72))); g.drawLine(P(-0.5,0.5),P(0.5,-0.5)); }
        else if(name=="nBreak"){ g.drawLine(P(-0.8,0.2),P(-0.2,0.2)); g.drawLine(P(0.2,-0.2),P(0.8,-0.2)); g.setBrush(col); g.setPen(Qt::NoPen); g.drawEllipse(P(-0.2,0.2),2.1,2.1); g.drawEllipse(P(0.2,-0.2),2.1,2.1); }
        else if(name=="nJoin"){ QPainterPath p; p.moveTo(P(-0.8,0.35)); p.cubicTo(P(-0.2,0.35),P(0.2,-0.35),P(0.8,-0.35)); g.drawPath(p); g.setBrush(col); g.setPen(Qt::NoPen); g.drawEllipse(P(-0.8,0.35),2.1,2.1); g.drawEllipse(P(0.8,-0.35),2.1,2.1); }
        else if(name=="nReverse"){ QRectF bx(P(-0.62,-0.62),P(0.62,0.62)); QPainterPath p; p.arcMoveTo(bx,310); p.arcTo(bx,310,-250); g.drawPath(p); QPointF t=p.currentPosition(); g.drawLine(t,t+QPointF(-0.06*r,-0.34*r)); g.drawLine(t,t+QPointF(0.32*r,-0.1*r)); }
        else if(name=="nToPath"){ QPainterPath p; p.moveTo(P(-0.82,0.42)); p.cubicTo(P(-0.3,-0.72),P(0.3,0.92),P(0.82,-0.42)); g.drawPath(p); g.setPen(QPen(col,1.3)); g.setBrush(Qt::NoBrush); for(QPointF v:{P(-0.82,0.42),P(0.0,0.05),P(0.82,-0.42)}) g.drawRect(QRectF(v.x()-2.4,v.y()-2.4,4.8,4.8)); }
        g.restore();
    }
}

void Canvas::radialBuildItems(){
    m_radialInner.clear(); m_radialOuter.clear();
    // Inner ring — the tool palette (a curated subset; releasing on one switches to it).
    struct TE{ Tool t; const char* label; };
    static const TE tools[]={ {Tool::Select,"Select"},{Tool::Node,"Edit"},{Tool::Rect,"Rect"},{Tool::Ellipse,"Ellipse"},
                              {Tool::Polygon,"Poly"},{Tool::Pen,"Pen"},{Tool::Text,"Text"},{Tool::Fill,"Fill"} };
    for(const TE& te : tools){ Tool t=te.t; m_radialInner.push_back({ QString::fromLatin1(te.label), [this,t]{ setTool(t); }, m_tool==t, int(t) }); }

    // Outer ring — the ACTIVE tool's quick options (each with a purpose-drawn glyph, see rmGlyph).
    auto add=[&](const QString& l, const QString& icon, std::function<void()> fn){ m_radialOuter.push_back({ l, std::move(fn), false, -1, icon }); };
    auto sel=[this](std::function<void(Shape&)> fn, const QString& name){ applyToSelection(fn, name); };
    // Apply an edit ONLY to selected shapes of a given type — and skip the undo + give feedback when none match,
    // so e.g. the Rectangle wedge fired over a non-rect selection doesn't push a no-op undo step.
    auto selT=[this](ShapeType t, std::function<void(Shape&)> fn, const QString& name){
        bool any=false; for(int i : m_sel) if(i>=0 && i<m_doc.shapes.size() && m_doc.shapes[i].type==t){ any=true; break; }
        if(!any){ emit statusMessage("That option needs a matching object selected."); return; }
        applyToSelection([t,fn](Shape& s){ if(s.type==t) fn(s); }, name);
    };
    switch(m_tool){
    case Tool::Rect:
        add("Sharp",   "rect0", [this,selT]{ selT(ShapeType::Rect, [](Shape& s){ s.cornerRadius=0; }, "Corner radius"); });
        add("Round S", "rect1", [this,selT]{ selT(ShapeType::Rect, [](Shape& s){ s.cornerRadius=qMin(s.rect.width(),s.rect.height())*0.12; }, "Corner radius"); });
        add("Round M", "rect2", [this,selT]{ selT(ShapeType::Rect, [](Shape& s){ s.cornerRadius=qMin(s.rect.width(),s.rect.height())*0.28; }, "Corner radius"); });
        add("Round L", "rect3", [this,selT]{ selT(ShapeType::Rect, [](Shape& s){ s.cornerRadius=qMin(s.rect.width(),s.rect.height())*0.5;  }, "Corner radius"); });
        break;
    case Tool::Ellipse:
        add("Whole",   "ellWhole", [this,selT]{ selT(ShapeType::Ellipse, [](Shape& s){ s.ellipseMode=0; s.ellipseSweep=360; }, "Ellipse mode"); });
        add("Pie",     "ellPie",   [this,selT]{ selT(ShapeType::Ellipse, [](Shape& s){ s.ellipseMode=1; if(s.ellipseSweep>=360)s.ellipseSweep=270; }, "Ellipse mode"); });
        add("Arc",     "ellArc",   [this,selT]{ selT(ShapeType::Ellipse, [](Shape& s){ s.ellipseMode=2; if(s.ellipseSweep>=360)s.ellipseSweep=270; }, "Ellipse mode"); });
        add("Segment", "ellSeg",   [this,selT]{ selT(ShapeType::Ellipse, [](Shape& s){ s.ellipseMode=3; if(s.ellipseSweep>=360)s.ellipseSweep=270; }, "Ellipse mode"); });
        break;
    case Tool::Polygon:
        add("Sides −", "polyMinus",    [this,selT]{ selT(ShapeType::Polygon, [](Shape& s){ s.polySides=qMax(3,s.polySides-1); }, "Polygon sides"); });
        add("Sides +", "polyPlus",     [this,selT]{ selT(ShapeType::Polygon, [](Shape& s){ s.polySides=qMin(40,s.polySides+1); }, "Polygon sides"); });
        add("Star ⇄",  "polyStar",     [this,selT]{ selT(ShapeType::Polygon, [](Shape& s){ s.polyStar=!s.polyStar; }, "Star"); });
        add("Straight","polyStraight", [this,selT]{ selT(ShapeType::Polygon, [](Shape& s){ s.polyCurvature=0; }, "Curvature"); });
        break;
    case Tool::Text: {
        // Toggle/scale the base font AND every rich run in lock-step, so styled (multi-run) text responds too.
        auto grow=[](Shape& s,double f){ auto gf=[f](QFont& fo){ if(fo.pointSizeF()>0) fo.setPointSizeF(qBound(4.0,fo.pointSizeF()*f,800.0));
                                                                 else if(fo.pixelSize()>0) fo.setPixelSize(qBound(4,int(fo.pixelSize()*f),800)); };
            gf(s.font); for(auto& r:s.textRuns) gf(r.font); };
        add("Bold",    "bold",    [this,selT]{ selT(ShapeType::Text, [](Shape& s){ bool b=!s.font.bold();   s.font.setBold(b);   for(auto& r:s.textRuns) r.font.setBold(b); },   "Bold"); });
        add("Italic",  "italic",  [this,selT]{ selT(ShapeType::Text, [](Shape& s){ bool i=!s.font.italic(); s.font.setItalic(i); for(auto& r:s.textRuns) r.font.setItalic(i); }, "Italic"); });
        add("Bigger",  "bigger",  [this,selT,grow]{ selT(ShapeType::Text, [grow](Shape& s){ grow(s,1.15); }, "Text size"); });
        add("Smaller", "smaller", [this,selT,grow]{ selT(ShapeType::Text, [grow](Shape& s){ grow(s,0.87); }, "Text size"); });
        break; }
    case Tool::Fill:   // seed a real 2-stop gradient from the object's own colour, else it snaps to the hardcoded default blue
        add("Flat",   "fillFlat",   [this,sel]{ sel([](Shape& s){ s.fill.color=s.fill.representativeColor(); s.fill.type=FillType::Solid; }, "Fill type"); });
        add("Linear", "fillLinear", [this,sel]{ sel([](Shape& s){ QColor c=s.fill.representativeColor(); s.fill.type=FillType::Linear; s.fill.stops={ {0.0,c},{1.0,c.darker(240)} }; }, "Fill type"); });
        add("Radial", "fillRadial", [this,sel]{ sel([](Shape& s){ QColor c=s.fill.representativeColor(); s.fill.type=FillType::Radial; s.fill.stops={ {0.0,c},{1.0,c.darker(240)} }; }, "Fill type"); });
        add("None",   "fillNone",   [this,sel]{ sel([](Shape& s){ s.fill.type=FillType::None; }, "Fill type"); });
        break;
    case Tool::Node:
        add("Break",   "nBreak",   [this]{ breakSelectedNodes(); });
        add("Join",    "nJoin",    [this]{ joinSelectedNodes(); });
        add("Reverse", "nReverse", [this]{ reverseNodePath(); });
        add("To Path", "nToPath",  [this]{ convertToPath(); });
        break;
    default:   // Select and every other tool: object arrangement acting on the current selection
        add("Group",     "group",   [this]{ groupSelection(); });
        add("Ungroup",   "ungroup", [this]{ ungroupSelection(); });
        add("Duplicate", "dup",     [this]{ duplicateSelection(); });
        add("Delete",    "delete",  [this]{ deleteSelection(); });
        add("To Front",  "front",   [this]{ toFront(); });
        add("To Back",   "back",    [this]{ toBack(); });
        break;
    }
}

void Canvas::radialArm(){
    // Called only when Ctrl+Alt are both held (see keyPressEvent). Never mid-drag, mid-edit, or in a modal mode.
    if(m_radialActive || m_dragging || m_panning || m_cropMode || m_straightenMode || editingText() ||
       m_penActive || (QApplication::mouseButtons()!=Qt::NoButton)) return;
    if(!m_radialTimer){ m_radialTimer=new QTimer(this); m_radialTimer->setSingleShot(true);
        connect(m_radialTimer,&QTimer::timeout,this,[this]{ radialShowAtCursor(); }); }
    // ⚠️ IDEMPOTENT: only start if not already pending. Some X11 setups deliver a held key's AUTOREPEAT as
    // press/release pairs with isAutoRepeat()==false, so radialArm fires every ~30ms — restarting a fresh 200ms
    // timer each time means it NEVER reaches timeout on a static hold (the pie only bloomed when mouse movement
    // happened to perturb the timing). Keep the first timer running so it fires.
    if(!m_radialTimer->isActive()) m_radialTimer->start(RM_HOLD_MS);
}
void Canvas::radialDisarm(){
    if(m_radialTimer) m_radialTimer->stop();
    if(m_radialActive) radialHide(false);
}
void Canvas::radialShowAtCursor(){
    // Re-check the world after the hold: Ctrl+Alt must still both be down and nothing may have started meanwhile.
    // ⚠️ Use a GEOMETRY test (cursor within the canvas rect), NOT underMouse(): Qt only refreshes WA_UnderMouse on
    // enter/leave/move events, so a perfectly STATIC cursor can read underMouse()==false and the pie never blooms
    // until the user jiggles the mouse. mapFromGlobal(QCursor::pos()) is always current.
    const QPointF cur = mapFromGlobal(QCursor::pos());
    // Live physical state (queryKeyboardModifiers), not the cached keyboardModifiers(): a spurious autorepeat
    // release just before the timer fires could otherwise leave the cache showing Ctrl/Alt as up.
    const auto mods=QApplication::queryKeyboardModifiers();
    if(!((mods&Qt::ControlModifier) && (mods&Qt::AltModifier)) || QApplication::mouseButtons()!=Qt::NoButton ||
       m_dragging || m_panning || m_cropMode || m_straightenMode || editingText() || m_penActive
       || !rect().contains(cur.toPoint())) return;
    radialBuildItems();
    QPointF c = cur;
    const double m = RM_ACT_OUT + 8;                              // keep the whole pie on-screen
    c.setX(qBound(m, c.x(), width()-m)); c.setY(qBound(m, c.y(), height()-m));
    m_radialCenter=c; m_radialRing=-1; m_radialIndex=-1; m_radialActive=true;
    update();
}
int Canvas::radialHitTest(const QPointF& v, int& ring) const {
    const QPointF d=v-m_radialCenter; const double dist=std::hypot(d.x(),d.y());
    if(dist<RM_HOLE){ ring=-1; return -1; }
    double ang=std::atan2(d.y(),d.x())*180.0/M_PI + 90.0;        // 0 at 12 o'clock, clockwise
    while(ang<0)ang+=360; while(ang>=360)ang-=360;
    if(dist<RM_TOOLS_OUT && !m_radialInner.isEmpty()){ ring=0; return qBound(0,int(ang/(360.0/m_radialInner.size())),m_radialInner.size()-1); }
    if(!m_radialOuter.isEmpty()){ ring=1; return qBound(0,int(ang/(360.0/m_radialOuter.size())),m_radialOuter.size()-1); }
    ring=-1; return -1;
}
void Canvas::radialHover(const QPointF& v){
    if(!m_radialActive) return;
    int ring=-1; int idx=radialHitTest(v,ring);
    if(ring!=m_radialRing || idx!=m_radialIndex){ m_radialRing=ring; m_radialIndex=idx; update(); }
}
void Canvas::radialHide(bool activate){
    std::function<void()> act;
    if(activate && m_radialRing==0 && m_radialIndex>=0 && m_radialIndex<m_radialInner.size()) act=m_radialInner[m_radialIndex].action;
    else if(activate && m_radialRing==1 && m_radialIndex>=0 && m_radialIndex<m_radialOuter.size()) act=m_radialOuter[m_radialIndex].action;
    m_radialActive=false; m_radialRing=-1; m_radialIndex=-1; m_radialInner.clear(); m_radialOuter.clear();
    update();
    if(act) act();   // run AFTER tearing the menu down, so the action sees a clean state
}

void Canvas::radialForceClose(){ if(m_radialTimer) m_radialTimer->stop(); if(m_radialActive) radialHide(false); }   // --selftest helper (QTimer complete here)
void Canvas::openRadialForShot(int hoverRing, int hoverIdx){   // debug/screenshot only (see XARA_RADIAL in mainwindow)
    radialBuildItems();
    m_radialCenter=QPointF(width()/2.0, height()/2.0);
    m_radialRing=hoverRing; m_radialIndex=hoverIdx; m_radialActive=true; update();
}
void Canvas::drawRadialMenu(QPainter& g){
    if(!m_radialActive) return;
    g.save();
    g.setRenderHint(QPainter::Antialiasing,true);
    const QPointF c=m_radialCenter;
    // Soft floating backdrop.
    g.setPen(Qt::NoPen);
    g.setBrush(QColor(0,0,0,70)); g.drawEllipse(c, RM_ACT_OUT+6, RM_ACT_OUT+6);       // faint shadow ring
    g.setBrush(QColor(24,27,33,242)); g.drawEllipse(c, RM_ACT_OUT+2, RM_ACT_OUT+2);   // disk
    g.setPen(QPen(QColor(255,255,255,26),1)); g.setBrush(Qt::NoBrush); g.drawEllipse(c, RM_ACT_OUT+2, RM_ACT_OUT+2);

    auto ring=[&](const QVector<RadialItem>& items, double rIn, double rOut, int ringId){
        const int n=items.size(); if(!n) return; const double step=360.0/n;
        QFont f=g.font(); f.setPixelSize(11); f.setBold(false); g.setFont(f);
        for(int i=0;i<n;++i){
            const double a0=-90.0+i*step, a1=a0+step, am=(a0+a1)*0.5;
            const bool hot=(m_radialRing==ringId && m_radialIndex==i);
            QPainterPath w=rmWedge(c,rIn+1.0,rOut-1.0,a0+0.8,a1-0.8);
            QColor fill = hot ? QColor(70,150,245,235)
                        : items[i].current ? QColor(70,150,245,60)
                        : ringId==0 ? QColor(255,255,255,14) : QColor(255,255,255,8);
            g.setPen(Qt::NoPen); g.setBrush(fill); g.drawPath(w);
            if(items[i].current && !hot){ g.setPen(QPen(QColor(90,165,250,200),1.4)); g.setBrush(Qt::NoBrush); g.drawPath(w); }
            const double rm=(rIn+rOut)*0.5, ar=am*M_PI/180.0;
            const QPointF tp=c+QPointF(std::cos(ar)*rm,std::sin(ar)*rm);
            const QColor fg = hot? QColor(255,255,255) : items[i].current? QColor(150,190,250) : QColor(214,220,228);
            if(items[i].toolId>=0){                                   // inner ring → the tool's own icon
                const int isz=22; QRect ir(int(tp.x())-isz/2, int(tp.y())-isz/2, isz, isz);
                toolIcon(Tool(items[i].toolId), isz, fg).paint(&g, ir, Qt::AlignCenter);
            } else if(!items[i].iconName.isEmpty()){                  // outer ring → its purpose-drawn glyph
                rmGlyph(g, items[i].iconName, tp, 11.0, fg);
            } else {                                                  // fallback → the text label
                g.setPen(fg); g.drawText(QRectF(tp.x()-rm, tp.y()-9, rm*2, 18), Qt::AlignCenter, items[i].label);
            }
        }
    };
    ring(m_radialInner, RM_HOLE, RM_TOOLS_OUT, 0);
    ring(m_radialOuter, RM_TOOLS_OUT, RM_ACT_OUT, 1);
    // separator between the rings + hole
    g.setPen(QPen(QColor(255,255,255,30),1)); g.setBrush(Qt::NoBrush);
    g.drawEllipse(c, RM_TOOLS_OUT, RM_TOOLS_OUT);
    g.setBrush(QColor(30,33,40,255)); g.setPen(QPen(QColor(255,255,255,22),1)); g.drawEllipse(c, RM_HOLE, RM_HOLE);
    // Tooltip: the hovered item's name in a pill just outside the pie (the tool ring is now iconic, so this is
    // where its name is revealed). Flip above / clamp horizontally so it never runs off the edge.
    QString tip = (m_radialRing==0 && m_radialIndex>=0 && m_radialIndex<m_radialInner.size()) ? m_radialInner[m_radialIndex].label
                : (m_radialRing==1 && m_radialIndex>=0 && m_radialIndex<m_radialOuter.size()) ? m_radialOuter[m_radialIndex].label
                : QString();
    if(!tip.isEmpty()){
        QFont tf=g.font(); tf.setPixelSize(12); tf.setBold(true); g.setFont(tf);
        const double tw=g.fontMetrics().horizontalAdvance(tip)+22, th=24;
        double py=c.y()+RM_ACT_OUT+12; if(py+th>height()-4) py=c.y()-RM_ACT_OUT-12-th;
        double px=qBound(4.0, c.x()-tw/2.0, qMax(4.0, width()-tw-4.0));
        QRectF pill(px, py, tw, th);
        g.setPen(Qt::NoPen); g.setBrush(QColor(12,14,18,240)); g.drawRoundedRect(pill,7,7);
        g.setPen(QPen(QColor(255,255,255,32),1)); g.setBrush(Qt::NoBrush); g.drawRoundedRect(pill,7,7);
        g.setPen(QColor(235,238,242)); g.drawText(pill, Qt::AlignCenter, tip);
    }
    g.restore();
}
